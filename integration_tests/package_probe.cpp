#include "chwell/storage/storage_factory.h"
#include "chwell/net/tls.h"
#ifdef PACKAGE_HAS_CONSUL
#include "chwell/discovery/consul_discovery.h"
#endif
#ifdef PACKAGE_HAS_PROTOBUF
#include "game.pb.h"
#endif

int main() {
    // StorageFactory pulls in MySQL/Mongo implementations, without connecting to a DB.
    auto storage = chwell::storage::StorageFactory::create("memory");
    if (!storage || !storage->put("probe", "value").ok) return 1;
    chwell::net::TlsContext tls; // Pull in OpenSSL when enabled.
#ifdef PACKAGE_HAS_CONSUL
    auto discovery = chwell::discovery::make_consul_discovery({}); // No registry request.
    if (!discovery) return 1;
#endif
#ifdef PACKAGE_HAS_PROTOBUF
    chwell::game::C2S_Login login;
    login.set_player_id("probe");
    if (login.SerializeAsString().empty()) return 1;
#endif
    return 0;
}
