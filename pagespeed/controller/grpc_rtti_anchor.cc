// Force the linker to keep gRPC RTTI symbols that are needed at runtime
// by the controller's RPC layer. Without this, the shared library linker
// strips typeinfo for gRPC classes because no code directly references
// their RTTI (only used via dynamic_cast/virtual dispatch).
//
// This file is compiled with alwayslink = True in the controller target.

#include <grpcpp/server_context.h>

#include <typeinfo>

namespace net_instaweb {
// volatile prevents the compiler from optimizing away the typeid references.
// These force the linker to pull in the gRPC RTTI archive members.
volatile const std::type_info* grpc_rtti_anchors[] = {
    &typeid(grpc::ServerContext),
    &typeid(grpc::ServerContextBase),
};
}  // namespace net_instaweb
