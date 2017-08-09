// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>

#include <iostream>
#include <set>
#include <string>

#include "apps/dart_content_handler/embedder/snapshot.h"
#include "dart/runtime/include/dart_api.h"
#include "lib/ftl/arraysize.h"
#include "lib/ftl/command_line.h"
#include "lib/ftl/files/directory.h"
#include "lib/ftl/files/eintr_wrapper.h"
#include "lib/ftl/files/file.h"
#include "lib/ftl/files/file_descriptor.h"
#include "lib/ftl/files/symlink.h"
#include "lib/ftl/files/unique_fd.h"
#include "lib/ftl/logging.h"
#include "lib/tonic/converter/dart_converter.h"
#include "lib/tonic/file_loader/file_loader.h"

namespace dart_snapshotter {
namespace {

using tonic::ToDart;

constexpr char kHelp[] = "help";
constexpr char kPackages[] = "packages";
constexpr char kSnapshot[] = "snapshot";
constexpr char kDepfile[] = "depfile";
constexpr char kBuildOutput[] = "build-output";
constexpr char kAOTVMSnapshot[] = "aot-vm-snapshot";

const char* kDartVMArgs[] = {
// clang-format off
#if defined(AOT_COMPILER)
    "--precompilation",
#else
    "--enable_mirrors=false",
#endif
    // clang-format on
};

void Usage() {
  std::cerr
      << "Usage: dart_snapshotter --" << kPackages << "=PACKAGES" << std::endl
      << "                      [ --" << kSnapshot << "=OUTPUT_SNAPSHOT ]"
      << std::endl
      << "                      [ --" << kDepfile << "=DEPFILE ]" << std::endl
      << "                      [ --" << kBuildOutput << "=BUILD_OUTPUT ]"
      << "                      [ --" << kAOTVMSnapshot << "]"
      << "                        MAIN_DART" << std::endl
      << std::endl
      << " --" << kAOTVMSnapshot << " selects generation of a VM snapshot that "
         "will be compatible with application snapshots generated by this "
         "tool. Does not require a MAIN_DART."
      << std::endl
      << " * PACKAGES is the '.packages' file that defines where to find Dart "
         "packages."
      << std::endl
      << " * OUTPUT_SNAPSHOT is the file to write the snapshot into."
      << std::endl
      << " * DEPFILE is the file into which to write the '.d' depedendency "
         "information into."
      << std::endl
      << " * BUILD_OUTPUT determines the target name used in the " << std::endl
      << "   DEPFILE. (Required if DEPFILE is provided.) " << std::endl;
}

class DartScope {
 public:
  DartScope(Dart_Isolate isolate) {
    Dart_EnterIsolate(isolate);
    Dart_EnterScope();
  }

  ~DartScope() {
    Dart_ExitScope();
    Dart_ExitIsolate();
  }
};

void InitDartVM() {
  FTL_CHECK(Dart_SetVMFlags(arraysize(kDartVMArgs), kDartVMArgs));
  Dart_InitializeParams params = {};
  params.version = DART_INITIALIZE_PARAMS_CURRENT_VERSION;
  params.vm_snapshot_data = dart_content_handler::vm_isolate_snapshot_buffer;
  char* error = Dart_Initialize(&params);
  if (error)
    FTL_LOG(FATAL) << error;
}

Dart_Isolate CreateDartIsolate() {
  FTL_CHECK(dart_content_handler::isolate_snapshot_buffer);
  char* error = nullptr;
  Dart_Isolate isolate = Dart_CreateIsolate(
      "dart:snapshot", "main", dart_content_handler::isolate_snapshot_buffer,
      nullptr, nullptr, nullptr, &error);
  FTL_CHECK(isolate) << error;
  Dart_ExitIsolate();
  return isolate;
}

tonic::FileLoader* g_loader = nullptr;

tonic::FileLoader& GetLoader() {
  if (!g_loader)
    g_loader = new tonic::FileLoader();
  return *g_loader;
}

Dart_Handle HandleLibraryTag(Dart_LibraryTag tag,
                             Dart_Handle library,
                             Dart_Handle url) {
  FTL_CHECK(Dart_IsLibrary(library));
  FTL_CHECK(Dart_IsString(url));
  tonic::FileLoader& loader = GetLoader();
  if (tag == Dart_kCanonicalizeUrl)
    return loader.CanonicalizeURL(library, url);
  if (tag == Dart_kImportTag)
    return loader.Import(url);
  if (tag == Dart_kSourceTag)
    return loader.Source(library, url);
  return Dart_NewApiError("Unknown library tag.");
}

std::vector<char> CreateSnapshot() {
#if !defined(AOT_COMPILER)
  uint8_t* buffer = nullptr;
  intptr_t size = 0;
  DART_CHECK_VALID(Dart_CreateScriptSnapshot(&buffer, &size));
  const char* begin = reinterpret_cast<const char*>(buffer);
  return std::vector<char>(begin, begin + size);
#else
  DART_CHECK_VALID(Dart_FinalizeLoading(false));

  // Import dart:_internal into dart:fuchsia.builtin for setting up hooks.
  Dart_Handle builtin_lib = Dart_LookupLibrary(ToDart("dart:fuchsia.builtin"));
  Dart_Handle internal_lib = Dart_LookupLibrary(ToDart("dart:_internal"));
  DART_CHECK_VALID(
      Dart_LibraryImportLibrary(builtin_lib, internal_lib, Dart_Null()));

  Dart_QualifiedFunctionName content_handler_entry_points[] = {
      {"dart:async", "::", "_setScheduleImmediateClosure"},
      {"dart:core", "::", "_uriBaseClosure"},
      {"dart:fidl.internal", "::", "_environment"},
      {"dart:fidl.internal", "::", "_outgoingServices"},
      {"dart:fuchsia.builtin", "::", "_getPrintClosure"},
      {"dart:fuchsia.builtin", "::", "_getScheduleMicrotaskClosure"},
      {"dart:fuchsia.builtin", "::", "_getUriBaseClosure"},
      {"dart:fuchsia.builtin", "::", "_rawScript"},
      {"dart:fuchsia.builtin", "::", "_rawUriBase"},
      {"dart:fuchsia.builtin", "::", "_setupHooks"},
      {"dart:io", "::", "_getWatchSignalInternal"},
      {"dart:io", "::", "_makeDatagram"},
      {"dart:io", "::", "_makeUint8ListView"},
      {"dart:io", "::", "_setupHooks"},
      {"dart:io", "::", "_setupHooks"},
      {"dart:io", "CertificateException", "CertificateException."},
      {"dart:io", "Directory", "Directory."},
      {"dart:io", "File", "File."},
      {"dart:io", "FileSystemException", "FileSystemException."},
      {"dart:io", "HandshakeException", "HandshakeException."},
      {"dart:io", "Link", "Link."},
      {"dart:io", "OSError", "OSError."},
      {"dart:io", "TlsException", "TlsException."},
      {"dart:io", "X509Certificate", "X509Certificate._"},
      {"dart:io", "_ExternalBuffer", "get:end"},
      {"dart:io", "_ExternalBuffer", "get:start"},
      {"dart:io", "_ExternalBuffer", "set:data"},
      {"dart:io", "_ExternalBuffer", "set:end"},
      {"dart:io", "_ExternalBuffer", "set:start"},
      {"dart:io", "_Platform", "set:_nativeScript"},
      {"dart:io", "_ProcessStartStatus", "set:_errorCode"},
      {"dart:io", "_ProcessStartStatus", "set:_errorMessage"},
      {"dart:io", "_SecureFilterImpl", "get:ENCRYPTED_SIZE"},
      {"dart:io", "_SecureFilterImpl", "get:SIZE"},
      {"dart:io", "_SecureFilterImpl", "get:buffers"},
      {"dart:isolate", "::", "_setupHooks"},
      {"::", "::", "main"},
      {NULL, NULL, NULL}  // Must be terminated with NULL entries.
  };
  DART_CHECK_VALID(Dart_Precompile(content_handler_entry_points, NULL, 0));

  uint8_t* buffer = nullptr;
  intptr_t size = 0;
  DART_CHECK_VALID(Dart_CreateAppAOTSnapshotAsAssembly(&buffer, &size));
  const char* begin = reinterpret_cast<const char*>(buffer);
  return std::vector<char>(begin, begin + size);
#endif
}

bool WriteDepfile(const std::string& path,
                  const std::string& build_output,
                  const std::set<std::string>& deps) {
  std::string current_directory = files::GetCurrentDirectory();
  std::string output = build_output + ":";
  for (const auto& dep : deps) {
    std::string file = dep;
    FTL_DCHECK(!file.empty());
    if (file[0] != '/')
      file = current_directory + "/" + file;

    std::string resolved_file;
    if (files::ReadSymbolicLink(file, &resolved_file)) {
      output += " " + resolved_file;
    } else {
      output += " " + file;
    }
  }
  return files::WriteFile(path, output.data(), output.size());
}

int CreateAOTVMSnapshot(std::string snapshot_path) {
  InitDartVM();

  Dart_Isolate isolate = CreateDartIsolate();
  FTL_CHECK(isolate) << "Failed to create isolate.";

  DartScope scope(isolate);

  uint8_t* snapshot_buffer = NULL;
  intptr_t snapshot_size = 0;
  DART_CHECK_VALID(
      Dart_CreateVMAOTSnapshotAsAssembly(&snapshot_buffer, &snapshot_size));

  if (!files::WriteFile(snapshot_path, reinterpret_cast<char*>(snapshot_buffer),
                        snapshot_size)) {
    std::cerr << "error: Failed to write snapshot to '" << snapshot_path << "'."
              << std::endl;
    return 1;
  }

  return 0;
}

int CreateSnapshot(const ftl::CommandLine& command_line) {
  if (command_line.HasOption(kHelp, nullptr)) {
    Usage();
    return 0;
  }

  std::string vm_snapshot_path;
  if (command_line.GetOptionValue(kAOTVMSnapshot, &vm_snapshot_path)) {
    return CreateAOTVMSnapshot(vm_snapshot_path);
  }

  if (command_line.positional_args().empty()) {
    Usage();
    return 1;
  }

  std::string packages;
  if (!command_line.GetOptionValue(kPackages, &packages)) {
    std::cerr << "error: Need --" << kPackages << std::endl;
    return 1;
  }

  std::vector<std::string> args = command_line.positional_args();
  if (args.size() != 1) {
    std::cerr << "error: Need one position argument. Got " << args.size() << "."
              << std::endl;
    return 1;
  }

  std::string main_dart = args[0];

  std::string snapshot;
  command_line.GetOptionValue(kSnapshot, &snapshot);

  if (snapshot.empty()) {
    std::cerr << "error: Need --" << kSnapshot << "." << std::endl;
    return 1;
  }

  std::string depfile;
  std::string build_output;
  if (command_line.GetOptionValue(kDepfile, &depfile) &&
      !command_line.GetOptionValue(kBuildOutput, &build_output)) {
    std::cerr << "error: Need --" << kBuildOutput << " if --" << kDepfile
              << " is specified." << std::endl;
    return 1;
  }

  InitDartVM();

  tonic::FileLoader& loader = GetLoader();
  if (!loader.LoadPackagesMap(packages))
    return 1;

  Dart_Isolate isolate = CreateDartIsolate();
  FTL_CHECK(isolate) << "Failed to create isolate.";

  DartScope scope(isolate);

  DART_CHECK_VALID(Dart_SetLibraryTagHandler(HandleLibraryTag));
  DART_CHECK_VALID(Dart_LoadScript(ToDart(main_dart), Dart_Null(),
                                   ToDart(loader.Fetch(main_dart)), 0, 0));

  std::vector<char> snapshot_blob = CreateSnapshot();

  if (!snapshot.empty() &&
      !files::WriteFile(snapshot, snapshot_blob.data(), snapshot_blob.size())) {
    std::cerr << "error: Failed to write snapshot to '" << snapshot << "'."
              << std::endl;
    return 1;
  }

  if (!depfile.empty() &&
      !WriteDepfile(depfile, build_output, loader.dependencies())) {
    std::cerr << "error: Failed to write depfile to '" << depfile << "'."
              << std::endl;
    return 1;
  }

  return 0;
}

}  // namespace
}  // namespace dart_snapshotter

int main(int argc, const char* argv[]) {
  return dart_snapshotter::CreateSnapshot(
      ftl::CommandLineFromArgcArgv(argc, argv));
}
