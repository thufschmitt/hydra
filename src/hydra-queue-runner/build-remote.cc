#include <algorithm>
#include <cmath>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "derived-path.hh"
#include "nix/build-result.hh"
#include "callback.hh"
#include "legacy-ssh-store.hh"
#include "serve-protocol.hh"
#include "state.hh"
#include "util.hh"
#include "worker-protocol.hh"
#include "finally.hh"
#include "url.hh"

using namespace nix;


struct Child
{
    Pid pid;
    AutoCloseFD to, from;
};

static ref<LegacySSHStore> openStore(
    Machine::ptr machine,
    int stderrFD
)
{
    return make_ref<LegacySSHStore>(
        "ssh",
        machine->sshName,
        Store::Params{
          {"log-fd", std::to_string(stderrFD) },
          {"max-connections", "1" },
          {"ssh-key", machine->sshKey },
          {"system-features", concatStringsSep(",", machine->supportedFeatures) }
        }
    );
}

// FIXME: use Store::topoSortPaths().
StorePaths reverseTopoSortPaths(const std::map<StorePath, ValidPathInfo> & paths)
{
    StorePaths sorted;
    StorePathSet visited;

    std::function<void(const StorePath & path)> dfsVisit;

    dfsVisit = [&](const StorePath & path) {
        if (!visited.insert(path).second) return;

        auto info = paths.find(path);
        auto references = info == paths.end() ? StorePathSet() : info->second.references;

        for (auto & i : references)
            /* Don't traverse into paths that don't exist.  That can
               happen due to substitutes for non-existent paths. */
            if (i != path && paths.count(i))
                dfsVisit(i);

        sorted.push_back(path);
    };

    for (auto & i : paths)
        dfsVisit(i.first);

    return sorted;
}

std::pair<Path, AutoCloseFD> openLogFile(const std::string & logDir, const StorePath & drvPath)
{
    std::string base(drvPath.to_string());
    auto logFile = logDir + "/" + std::string(base, 0, 2) + "/" + std::string(base, 2);

    createDirs(dirOf(logFile));

    AutoCloseFD logFD = open(logFile.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (!logFD) throw SysError("creating log file ‘%s’", logFile);

    return {std::move(logFile), std::move(logFD)};
}

BasicDerivation sendInputs(
    State & state,
    Step & step,
    Store & localStore,
    Store & destStore,
    Machine::Connection & conn,
    unsigned int & overhead,
    counter & nrStepsWaiting,
    counter & nrStepsCopyingTo
)
{
    BasicDerivation basicDrv(*step.drv);

    for (auto & input : step.drv->inputDrvs) {
        auto drv2 = localStore.readDerivation(input.first);
        for (auto & name : input.second) {
            if (auto i = get(drv2.outputs, name)) {
                auto outPath = i->path(localStore, drv2.name, name);
                basicDrv.inputSrcs.insert(*outPath);
            }
        }
    }

    /* Ensure that the inputs exist in the destination store. This is
       a no-op for regular stores, but for the binary cache store,
       this will copy the inputs to the binary cache from the local
       store. */
    if (localStore.getUri() != destStore.getUri()) {
        StorePathSet closure;
        localStore.computeFSClosure(step.drv->inputSrcs, closure);
        copyPaths(localStore, destStore, closure, NoRepair, NoCheckSigs, NoSubstitute);
    }

    {
        auto mc1 = std::make_shared<MaintainCount<counter>>(nrStepsWaiting);
        mc1.reset();
        MaintainCount<counter> mc2(nrStepsCopyingTo);

        printMsg(lvlDebug, "sending closure of ‘%s’ to ‘%s’",
            localStore.printStorePath(step.drvPath), conn.machine->sshName);

        auto now1 = std::chrono::steady_clock::now();

        /* Copy the input closure. */
        if (conn.machine->isLocalhost()) {
            StorePathSet closure;
            destStore.computeFSClosure(basicDrv.inputSrcs, closure);
            copyPaths(destStore, localStore, closure, NoRepair, NoCheckSigs, NoSubstitute);
        } else {
            copyClosure(destStore, *conn.store, basicDrv.inputSrcs);
        }

        auto now2 = std::chrono::steady_clock::now();

        overhead += std::chrono::duration_cast<std::chrono::milliseconds>(now2 - now1).count();
    }

    return basicDrv;
}

void RemoteResult::updateWithBuildResult(const nix::BuildResult & buildResult)
{
    RemoteResult thisArrow;

    startTime = buildResult.startTime;
    stopTime = buildResult.stopTime;
    timesBuilt = buildResult.timesBuilt;
    errorMsg = buildResult.errorMsg;
    isNonDeterministic = buildResult.isNonDeterministic;

    switch ((BuildResult::Status) buildResult.status) {
        case BuildResult::Built:
            stepStatus = bsSuccess;
            break;
        case BuildResult::Substituted:
        case BuildResult::AlreadyValid:
            stepStatus = bsSuccess;
            isCached = true;
            break;
        case BuildResult::PermanentFailure:
            stepStatus = bsFailed;
            canCache = true;
            errorMsg = "";
            break;
        case BuildResult::InputRejected:
        case BuildResult::OutputRejected:
            stepStatus = bsFailed;
            canCache = true;
            break;
        case BuildResult::TransientFailure:
            stepStatus = bsFailed;
            canRetry = true;
            errorMsg = "";
            break;
        case BuildResult::TimedOut:
            stepStatus = bsTimedOut;
            errorMsg = "";
            break;
        case BuildResult::MiscFailure:
            stepStatus = bsAborted;
            canRetry = true;
            break;
        case BuildResult::LogLimitExceeded:
            stepStatus = bsLogLimitExceeded;
            break;
        case BuildResult::NotDeterministic:
            stepStatus = bsNotDeterministic;
            canRetry = false;
            canCache = true;
            break;
        default:
            stepStatus = bsAborted;
            break;
    }

}

BuildResult performBuild(
    Machine::Connection & machineConn,
    Store & localStore,
    StorePath drvPath,
    const BasicDerivation & drv,
    const State::BuildOptions & options,
    counter & nrStepsBuilding
)
{

    BuildResult result{.path = DerivedPathBuilt{.drvPath = drvPath, .outputs = drv.outputNames()}};

    auto conn = machineConn.store->openConnection();

    conn->to << cmdBuildDerivation << localStore.printStorePath(drvPath);
    writeDerivation(conn->to, localStore, drv);
    conn->to << options.maxSilentTime << options.buildTimeout;
    if (GET_PROTOCOL_MINOR(conn->remoteVersion) >= 2)
        conn->to << options.maxLogSize;
    if (GET_PROTOCOL_MINOR(conn->remoteVersion) >= 3) {
        conn->to << options.repeats // == build-repeat
          << options.enforceDeterminism;
    }
    if (GET_PROTOCOL_MINOR(conn->remoteVersion) >= 7) {
        conn->to << ((int) false); // keep-failed
    }
    conn->to.flush();

    result.startTime = time(0);

    {
        MaintainCount<counter> mc(nrStepsBuilding);
        result.status = (BuildResult::Status)readInt(conn->from);
    }
    result.stopTime = time(0);


    result.errorMsg = readString(conn->from);
    if (GET_PROTOCOL_MINOR(conn->remoteVersion) >= 3) {
        result.timesBuilt = readInt(conn->from);
        result.isNonDeterministic = readInt(conn->from);
        auto start = readInt(conn->from);
        auto stop = readInt(conn->from);
        if (start && start) {
            /* Note: this represents the duration of a single
                round, rather than all rounds. */
            result.startTime = start;
            result.stopTime = stop;
        }
    }
    if (GET_PROTOCOL_MINOR(conn->remoteVersion) >= 6) {
        result.builtOutputs = worker_proto::read(localStore, conn->from, Phantom<DrvOutputs> {});
    }

    return result;
}

std::map<StorePath, ValidPathInfo> queryPathInfos(
    Machine::Connection & machineConn,
    Store & localStore,
    StorePathSet & outputs,
    size_t & totalNarSize
)
{

    auto conn = machineConn.store->openConnection();

    /* Get info about each output path. */
    std::map<StorePath, ValidPathInfo> infos;
    conn->to << cmdQueryPathInfos;
    worker_proto::write(localStore, conn->to, outputs);
    conn->to.flush();
    while (true) {
        auto storePathS = readString(conn->from);
        if (storePathS == "") break;
        auto deriver = readString(conn->from); // deriver
        auto references = worker_proto::read(localStore, conn->from, Phantom<StorePathSet> {});
        readLongLong(conn->from); // download size
        auto narSize = readLongLong(conn->from);
        auto narHash = Hash::parseAny(readString(conn->from), htSHA256);
        auto ca = parseContentAddressOpt(readString(conn->from));
        readStrings<StringSet>(conn->from); // sigs
        ValidPathInfo info(localStore.parseStorePath(storePathS), narHash);
        assert(outputs.count(info.path));
        info.references = references;
        info.narSize = narSize;
        totalNarSize += info.narSize;
        info.narHash = narHash;
        info.ca = ca;
        if (deriver != "")
            info.deriver = localStore.parseStorePath(deriver);
        infos.insert_or_assign(info.path, info);
    }

    return infos;
}

void copyPathFromRemote(
    Machine::Connection & machineConn,
    NarMemberDatas & narMembers,
    Store & localStore,
    Store & destStore,
    const ValidPathInfo & info
)
{

    auto conn = machineConn.store->openConnection();
      /* Receive the NAR from the remote and add it to the
          destination store. Meanwhile, extract all the info from the
          NAR that getBuildOutput() needs. */
      auto source2 = sinkToSource([&](Sink & sink)
      {
          /* Note: we should only send the command to dump the store
              path to the remote if the NAR is actually going to get read
              by the destination store, which won't happen if this path
              is already valid on the destination store. Since this
              lambda function only gets executed if someone tries to read
              from source2, we will send the command from here rather
              than outside the lambda. */
          conn->to << cmdDumpStorePath << localStore.printStorePath(info.path);
          conn->to.flush();

          TeeSource tee(conn->from, sink);
          extractNarData(tee, localStore.printStorePath(info.path), narMembers);
      });

      destStore.addToStore(info, *source2, NoRepair, NoCheckSigs);
}

void copyPathsFromRemote(
    Machine::Connection & conn,
    NarMemberDatas & narMembers,
    Store & localStore,
    Store & destStore,
    const std::map<StorePath, ValidPathInfo> & infos
)
{
      auto pathsSorted = reverseTopoSortPaths(infos);

      for (auto & path : pathsSorted) {
          auto & info = infos.find(path)->second;
          copyPathFromRemote(conn, narMembers, localStore, destStore, info);
      }

}


void State::buildRemote(ref<Store> destStore,
    Machine::ptr machine, Step::ptr step,
    const BuildOptions & buildOptions,
    RemoteResult & result, std::shared_ptr<ActiveStep> activeStep,
    std::function<void(StepState)> updateStep,
    NarMemberDatas & narMembers)
{
    assert(BuildResult::TimedOut == 8);

    auto [logFile, logFD] = openLogFile(logDir, step->drvPath);
    AutoDelete logFileDel(logFile, false);
    result.logFile = logFile;

    nix::Path tmpDir = createTempDir();
    AutoDelete tmpDirDel(tmpDir, true);

    try {

        updateStep(ssConnecting);

        // FIXME: rewrite to use Store.
        /* Child child; */
        /* openConnection(machine, tmpDir, logFD.get(), child); */
        auto sshStore = openStore(machine, logFD.get());

        {
            auto activeStepState(activeStep->state_.lock());
            if (activeStepState->cancelled) throw Error("step cancelled");
            // activeStepState->pid = child.pid;
        }

        Finally clearPid([&]() {
            auto activeStepState(activeStep->state_.lock());
            activeStepState->pid = -1;

            /* FIXME: there is a slight race here with step
               cancellation in State::processQueueChange(), which
               could call kill() on this pid after we've done waitpid()
               on it. With pid wrap-around, there is a tiny
               possibility that we end up killing another
               process. Meh. */
        });

        Machine::Connection machineConn {
            .store = sshStore,
            .machine = machine,
        };

        Finally updateStats([&]() {
            auto conn = sshStore->openConnection();
            bytesReceived += conn->from.read;
            bytesSent += conn->to.written;
        });

        {
            auto info(machine->state->connectInfo.lock());
            info->consecutiveFailures = 0;
        }

        /* Gather the inputs. If the remote side is Nix <= 1.9, we have to
           copy the entire closure of ‘drvPath’, as well as the required
           outputs of the input derivations. On Nix > 1.9, we only need to
           copy the immediate sources of the derivation and the required
           outputs of the input derivations. */
        updateStep(ssSendingInputs);
        BasicDerivation resolvedDrv = sendInputs(*this, *step, *localStore, *destStore, machineConn, result.overhead, nrStepsWaiting, nrStepsCopyingTo);

        logFileDel.cancel();

        /* Truncate the log to get rid of messages about substitutions
            etc. on the remote system. */
        if (lseek(logFD.get(), SEEK_SET, 0) != 0)
            throw SysError("seeking to the start of log file ‘%s’", result.logFile);

        if (ftruncate(logFD.get(), 0) == -1)
            throw SysError("truncating log file ‘%s’", result.logFile);

        logFD = -1;

        /* Do the build. */
        printMsg(lvlDebug, "building ‘%s’ on ‘%s’",
            localStore->printStorePath(step->drvPath),
            machine->sshName);

        updateStep(ssBuilding);

        BuildResult buildResult = performBuild(
            machineConn,
            *localStore,
            step->drvPath,
            resolvedDrv,
            buildOptions,
            nrStepsBuilding
        );

        result.updateWithBuildResult(buildResult);

        if (result.stepStatus != bsSuccess) return;

        result.errorMsg = "";

        /* If the path was substituted or already valid, then we didn't
           get a build log. */
        if (result.isCached) {
            printMsg(lvlInfo, "outputs of ‘%s’ substituted or already valid on ‘%s’",
                localStore->printStorePath(step->drvPath), machine->sshName);
            unlink(result.logFile.c_str());
            result.logFile = "";
        }

        /* Copy the output paths. */
        if (!machine->isLocalhost() || localStore != std::shared_ptr<Store>(destStore)) {
            updateStep(ssReceivingOutputs);

            MaintainCount<counter> mc(nrStepsCopyingFrom);

            auto now1 = std::chrono::steady_clock::now();

            StorePathSet outputs;
            for (auto & i : step->drv->outputsAndOptPaths(*localStore)) {
                if (i.second.second)
                   outputs.insert(*i.second.second);
            }

            size_t totalNarSize = 0;
            auto infos = queryPathInfos(machineConn, *localStore, outputs, totalNarSize);

            if (totalNarSize > maxOutputSize) {
                result.stepStatus = bsNarSizeLimitExceeded;
                return;
            }

            /* Copy each path. */
            printMsg(lvlDebug, "copying outputs of ‘%s’ from ‘%s’ (%d bytes)",
                localStore->printStorePath(step->drvPath), machine->sshName, totalNarSize);

            copyPathsFromRemote(machineConn, narMembers, *localStore, *destStore, infos);
            auto now2 = std::chrono::steady_clock::now();

            result.overhead += std::chrono::duration_cast<std::chrono::milliseconds>(now2 - now1).count();
        }

    } catch (Error & e) {
        /* Disable this machine until a certain period of time has
           passed. This period increases on every consecutive
           failure. However, don't count failures that occurred soon
           after the last one (to take into account steps started in
           parallel). */
        auto info(machine->state->connectInfo.lock());
        auto now = std::chrono::system_clock::now();
        if (info->consecutiveFailures == 0 || info->lastFailure < now - std::chrono::seconds(30)) {
            info->consecutiveFailures = std::min(info->consecutiveFailures + 1, (unsigned int) 4);
            info->lastFailure = now;
            int delta = retryInterval * std::pow(retryBackoff, info->consecutiveFailures - 1) + (rand() % 30);
            printMsg(lvlInfo, "will disable machine ‘%1%’ for %2%s", machine->sshName, delta);
            info->disabledUntil = now + std::chrono::seconds(delta);
        }
        throw;
    }
}
