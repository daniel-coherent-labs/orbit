//-----------------------------------
// Copyright Pierric Gimmig 2013-2017
//-----------------------------------

#include "ConnectionManager.h"

#include "Capture.h"
#include "ContextSwitch.h"
#include "CoreApp.h"
#include "EventBuffer.h"
#include "Introspection.h"
#include "KeyAndString.h"
#include "LinuxCallstackEvent.h"
#include "LinuxSymbol.h"
#include "OrbitBase/Logging.h"
#include "OrbitBase/Tracing.h"
#include "OrbitFunction.h"
#include "OrbitModule.h"
#include "Params.h"
#include "ProcessUtils.h"
#include "SamplingProfiler.h"
#include "Serialization.h"
#include "SymbolHelper.h"
#include "TcpClient.h"
#include "TcpServer.h"
#include "TestRemoteMessages.h"
#include "TimerManager.h"

#if __linux__
#include "LinuxUtils.h"
#include "OrbitLinuxTracing/OrbitTracing.h"
#endif

#include <fstream>
#include <iostream>
#include <sstream>
#include <streambuf>

ConnectionManager::ConnectionManager()
    : tracing_session_(GTcpServer.get()),
      exit_requested_(false),
      is_service_(false) {}

ConnectionManager::~ConnectionManager() {
  StopThread();
  StopCaptureAsRemote();
}

void ConnectionManager::StopThread() {
  if (thread_.joinable()) {
    exit_requested_ = true;
    thread_.join();
  }
}

ConnectionManager& ConnectionManager::Get() {
  static ConnectionManager instance;
  return instance;
}

void ConnectionManager::ConnectToRemote(std::string remote_address) {
  remote_address_ = remote_address;
  StopThread();
  SetupClientCallbacks();
  thread_ = std::thread{[this]() { ConnectionThreadWorker(); }};
}

void ConnectionManager::InitAsService() {
#if __linux__
  GParams.m_TrackContextSwitches = true;
#endif

  is_service_ = true;
  string_manager_ = std::make_shared<StringManager>();
  tracing_session_.SetStringManager(string_manager_);
  SetupIntrospection();
  SetupServerCallbacks();
  thread_ = std::thread{[this]() { RemoteThreadWorker(); }};
}

void ConnectionManager::SetSelectedFunctionsOnRemote(const Message& a_Msg) {
  PRINT_FUNC;
  const char* a_Data = a_Msg.GetData();
  size_t a_Size = a_Msg.m_Size;
  std::istringstream buffer(std::string(a_Data, a_Size));
  cereal::JSONInputArchive inputAr(buffer);
  std::vector<std::string> selectedFunctions;
  inputAr(selectedFunctions);

  // Unselect the all currently selected functions:
  std::vector<Function*> prevSelectedFuncs;
  for (auto& pair : Capture::GSelectedFunctionsMap) {
    prevSelectedFuncs.push_back(pair.second);
  }

  Capture::GSelectedFunctionsMap.clear();
  for (Function* function : prevSelectedFuncs) {
    if (function) function->UnSelect();
  }

  // Select the received functions:
  for (const std::string& address_str : selectedFunctions) {
    uint64_t address = std::stoll(address_str);
    PRINT("Select address %x\n", address);
    Function* function =
        Capture::GTargetProcess->GetFunctionFromAddress(address);
    if (!function)
      PRINT("Received invalid address %x\n", address);
    else {
      PRINT("Received Selected Function: %s\n", function->PrettyName().c_str());
      // this also adds the function to the map.
      function->Select();
    }
  }
}

void ConnectionManager::ServerCaptureThreadWorker() {
  while (Capture::IsCapturing()) {
    OrbitSleepMs(20);

    std::vector<Timer> timers;
    if (tracing_session_.ReadAllTimers(&timers)) {
      Message Msg(Msg_RemoteTimers);
      GTcpServer->Send(Msg, timers);
    }

    std::vector<LinuxCallstackEvent> callstacks;
    if (tracing_session_.ReadAllCallstacks(&callstacks)) {
      std::string message_data = SerializeObjectBinary(callstacks);
      GTcpServer->Send(Msg_SamplingCallstacks, message_data.c_str(),
                       message_data.size());
    }

    std::vector<CallstackEvent> hashed_callstacks;
    if (tracing_session_.ReadAllHashedCallstacks(&hashed_callstacks)) {
      std::string message_data = SerializeObjectBinary(hashed_callstacks);
      GTcpServer->Send(Msg_SamplingHashedCallstacks, message_data.c_str(),
                       message_data.size());
    }

    std::vector<ContextSwitch> context_switches;
    if (tracing_session_.ReadAllContextSwitches(&context_switches)) {
      Message Msg(Msg_RemoteContextSwitches);
      GTcpServer->Send(Msg, context_switches);
    }
  }
}

void ConnectionManager::SetupIntrospection() {
#if __linux__ && ORBIT_TRACING_ENABLED
  // Setup introspection handler.
  auto handler =
      std::make_unique<orbit::introspection::Handler>(&tracing_session_);
  LinuxTracing::SetOrbitTracingHandler(std::move(handler));
#endif  // ORBIT_TRACING_ENABLED
}

void ConnectionManager::StartCaptureAsRemote(uint32_t pid) {
  PRINT_FUNC;
  std::shared_ptr<Process> process = process_list_.GetProcess(pid);
  if (!process) {
    PRINT("Process not found (pid=%d)\n", pid);
    return;
  }
  Capture::SetTargetProcess(process);
  tracing_session_.Reset();
  string_manager_->Clear();
  Capture::StartCapture(&tracing_session_);
  server_capture_thread_ =
      std::thread{[this]() { ServerCaptureThreadWorker(); }};
}

void ConnectionManager::StopCaptureAsRemote() {
  PRINT_FUNC;
  // The thread checks Capture::IsCapturing() and exits main loop
  // when it is false. StopCapture should be called before joining
  // the thread.
  Capture::StopCapture();
  if (server_capture_thread_.joinable()) {
    server_capture_thread_.join();
  }
}

void ConnectionManager::Stop() { exit_requested_ = true; }

void ConnectionManager::SetupServerCallbacks() {
  GTcpServer->AddMainThreadCallback(
      Msg_RemoteSelectedFunctionsMap,
      [this](const Message& a_Msg) { SetSelectedFunctionsOnRemote(a_Msg); });

  GTcpServer->AddMainThreadCallback(
      Msg_StartCapture, [this](const Message& msg) {
        uint32_t pid =
            static_cast<uint32_t>(msg.m_Header.m_GenericHeader.m_Address);
        StartCaptureAsRemote(pid);
      });

  GTcpServer->AddMainThreadCallback(
      Msg_StopCapture, [this](const Message&) { StopCaptureAsRemote(); });

  GTcpServer->AddMainThreadCallback(
      Msg_RemoteProcessRequest, [this](const Message& msg) {
        uint32_t pid =
            static_cast<uint32_t>(msg.m_Header.m_GenericHeader.m_Address);

        SendRemoteProcess(GTcpServer.get(), pid);
      });

  GTcpServer->AddMainThreadCallback(
      Msg_RemoteModuleDebugInfo, [=](const Message& msg) {
        uint32_t pid =
            static_cast<uint32_t>(msg.m_Header.m_GenericHeader.m_Address);

        std::shared_ptr<Process> process = process_list_.GetProcess(pid);
        if (!process) {
          ERROR("Process not found (pid=%d)", pid);
          return;
        }

        Capture::SetTargetProcess(process);

        std::vector<ModuleDebugInfo> module_infos;
        std::istringstream buffer(std::string(msg.m_Data, msg.m_Size));
        cereal::BinaryInputArchive inputAr(buffer);
        inputAr(module_infos);

        std::vector<ModuleDebugInfo> module_infos_send_back;

        for (auto& module_info : module_infos) {
          std::shared_ptr<Module> module =
              process->GetModuleFromName(module_info.m_Name);
          if (!module) {
            ERROR("Unable to find module %s", module_info.m_Name.c_str());
            continue;
          }
          const SymbolHelper symbolHelper;
          if (!module_info.m_Functions.empty()) {
            LOG("Received %lu function symbols from local machine for "
                "module %s",
                module_info.m_Functions.size(), module_info.m_Name.c_str());
            symbolHelper.LoadSymbolsFromDebugInfo(module, module_info);
            continue;
          }

          if (symbolHelper.LoadSymbolsCollector(module)) {
            symbolHelper.FillDebugInfoFromModule(module, module_info);
            LOG("Loaded %lu function symbols for module %s",
                module_info.m_Functions.size(), module_info.m_Name.c_str());
          } else {
            ERROR("Unable to load symbols of module %s",
                  module->m_Name.c_str());
          }

          module_infos_send_back.emplace_back(module_info);
        }

        // Send data back
        std::string message_data =
            SerializeObjectBinary(module_infos_send_back);
        GTcpServer->Send(Msg_RemoteModuleDebugInfo, message_data);
      });
}

void ConnectionManager::SetupClientCallbacks() {
  GTcpClient->AddMainThreadCallback(Msg_RemotePerf, [=](const Message& a_Msg) {
    PRINT_VAR(a_Msg.m_Size);
    std::string msgStr(a_Msg.m_Data, a_Msg.m_Size);
    std::istringstream buffer(msgStr);

    Capture::NewSamplingProfiler();
    Capture::GSamplingProfiler->StartCapture();
    Capture::GSamplingProfiler->SetIsLinuxPerf(true);
    Capture::GSamplingProfiler->StopCapture();
    Capture::GSamplingProfiler->ProcessSamples();
    GCoreApp->RefreshCaptureView();
  });

  GTcpClient->AddCallback(Msg_SamplingCallstack, [=](const Message& a_Msg) {
    // TODO: Send buffered callstacks.
    LinuxCallstackEvent data;

    std::istringstream buffer(std::string(a_Msg.m_Data, a_Msg.m_Size));
    cereal::JSONInputArchive inputAr(buffer);
    inputAr(data);

    GCoreApp->ProcessSamplingCallStack(data);
  });

  GTcpClient->AddCallback(Msg_RemoteTimers, [=](const Message& a_Msg) {
    uint32_t numTimers = (uint32_t)a_Msg.m_Size / sizeof(Timer);
    Timer* timers = (Timer*)a_Msg.GetData();
    for (uint32_t i = 0; i < numTimers; ++i) {
      GTimerManager->Add(timers[i]);
    }
  });

  GTcpClient->AddCallback(Msg_KeyAndString, [=](const Message& a_Msg) {
    KeyAndString key_and_string;
    std::istringstream buffer(std::string(a_Msg.m_Data, a_Msg.m_Size));
    cereal::BinaryInputArchive inputAr(buffer);
    inputAr(key_and_string);
    GCoreApp->AddKeyAndString(key_and_string.key, key_and_string.str);
  });

  GTcpClient->AddCallback(Msg_RemoteCallStack, [=](const Message& a_Msg) {
    CallStack stack;
    std::istringstream buffer(std::string(a_Msg.m_Data, a_Msg.m_Size));
    cereal::JSONInputArchive inputAr(buffer);
    inputAr(stack);

    GCoreApp->ProcessCallStack(stack);
  });

  GTcpClient->AddCallback(Msg_RemoteSymbol, [=](const Message& a_Msg) {
    LinuxSymbol symbol;
    std::istringstream buffer(std::string(a_Msg.m_Data, a_Msg.m_Size));
    cereal::BinaryInputArchive inputAr(buffer);
    inputAr(symbol);

    GCoreApp->AddSymbol(symbol.m_Address, symbol.m_Module, symbol.m_Name);
  });

  GTcpClient->AddCallback(Msg_RemoteContextSwitches, [=](const Message& a_Msg) {
    uint32_t num_context_switches =
        (uint32_t)a_Msg.m_Size / sizeof(ContextSwitch);
    ContextSwitch* context_switches = (ContextSwitch*)a_Msg.GetData();
    for (uint32_t i = 0; i < num_context_switches; i++) {
      GCoreApp->ProcessContextSwitch(context_switches[i]);
    }
  });

  GTcpClient->AddCallback(Msg_SamplingCallstacks, [=](const Message& a_Msg) {
    const char* a_Data = a_Msg.GetData();
    size_t a_Size = a_Msg.m_Size;
    std::istringstream buffer(std::string(a_Data, a_Size));
    cereal::BinaryInputArchive inputAr(buffer);
    std::vector<LinuxCallstackEvent> call_stacks;
    inputAr(call_stacks);

    for (auto& cs : call_stacks) {
      GCoreApp->ProcessSamplingCallStack(cs);
    }
  });

  GTcpClient->AddCallback(
      Msg_SamplingHashedCallstacks, [=](const Message& a_Msg) {
        const char* a_Data = a_Msg.GetData();
        size_t a_Size = a_Msg.m_Size;
        std::istringstream buffer(std::string(a_Data, a_Size));
        cereal::BinaryInputArchive inputAr(buffer);
        std::vector<CallstackEvent> call_stacks;
        inputAr(call_stacks);

        for (auto& cs : call_stacks) {
          GCoreApp->ProcessHashedSamplingCallStack(cs);
        }
      });
}

void ConnectionManager::SendProcesses(TcpEntity* tcp_entity) {
  process_list_.Refresh();
  process_list_.UpdateCpuTimes();
  std::string process_data = SerializeObjectHumanReadable(process_list_);
  tcp_entity->Send(Msg_RemoteProcessList, process_data.data(),
                   process_data.size());
}

void ConnectionManager::SendRemoteProcess(TcpEntity* tcp_entity, uint32_t pid) {
  std::shared_ptr<Process> process = process_list_.GetProcess(pid);
  // TODO: remove this - pid should be part of every message,
  // and all the messages should to be as stateless as possible.
  Capture::SetTargetProcess(process);
  process->ListModules();
  process->EnumerateThreads();
  if (process) {
    std::string process_data = SerializeObjectHumanReadable(*process);
    tcp_entity->Send(Msg_RemoteProcess, process_data.data(),
                     process_data.size());
  }
}

void ConnectionManager::ConnectionThreadWorker() {
  while (!exit_requested_) {
    if (!GTcpClient->IsValid()) {
      GTcpClient->Stop();
      GTcpClient->Connect(remote_address_);
      GTcpClient->Start();
    } else {
      // std::string msg("Hello from dev machine");
      // GTcpClient->Send(msg);
    }

    Sleep(2000);
  }
}

void ConnectionManager::RemoteThreadWorker() {
  while (!exit_requested_) {
    if (GTcpServer && GTcpServer->HasConnection()) {
      SendProcesses(GTcpServer.get());
    }

    Sleep(2000);
  }
}
