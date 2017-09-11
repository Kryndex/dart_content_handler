// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_DART_CONTENT_HANDLER_EMBEDDER_BUILTIN_H_
#define APPS_DART_CONTENT_HANDLER_EMBEDDER_BUILTIN_H_

#include <memory>
#include <string>

#include <mxio/namespace.h>

#include "lib/app/cpp/application_context.h"
#include "lib/app/fidl/service_provider.fidl.h"

namespace dart_content_handler {

void InitBuiltinLibrariesForIsolate(
    const std::string& script_uri,
    mxio_ns_t* namespc,
    std::unique_ptr<app::ApplicationContext> context,
    fidl::InterfaceRequest<app::ServiceProvider> outgoing_services);

}  // namespace dart_content_handler

#endif  // APPS_DART_CONTENT_HANDLER_EMBEDDER_BUILTIN_H_
