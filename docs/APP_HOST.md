# AppHost and Startup Contracts

AppHost assembles an application from ChwellCore's existing Config format or a
typed AppManifest. It is an independent implementation of the design discussed
in ARK_COMPARISON.md; no ARK source is used.

```cpp
chwell::core::Config config;
if (!config.load_from_file("config/game_server.conf")) return 1;

chwell::service::AppHost host;
host.register_component_factory("Players", [](const auto& component_config) {
    // Validate business parameters here; acquire resources during Init.
    return std::make_unique<PlayerManager>(component_config.params);
});
if (!host.configure(config) || !host.start()) {
    std::cerr << host.last_error() << '\n';
    return 1;
}
host.update();
host.stop();
```

## Manifest

```ini
listen_port = 9000
worker_threads = 4
use_epoll = true
reactor_threads = 2
logic_workers = 2
component.Players.enabled = true
component.Players.priority = 10
component.Players.depends_on = Storage, Sessions
component.Players.max_players = 10000
component.Storage.enabled = true
component.Sessions.enabled = true
```

The configuration entry name selects a registered factory. Factories return
components with their existing runtime names, which may differ from the entry
name. Both manifest entry names and runtime component names must be unique.
Disabled entries need no factory and are never constructed. Component parameters
are passed to the factory unchanged; each factory owns its parameter validation.
Port/thread fields, enabled flags and priorities are parsed strictly. Port zero
is allowed for ephemeral test listeners; thread counts must be positive, except
logic_workers may be zero to select the reactor count.

AppHost validates all enabled manifest dependencies before invoking factories,
then constructs all components and validates their combined runtime graph before
creating the Service or registering components. Factories and registration follow
dependency order; among ready manifest entries, priority and entry name break ties.
Service uses the manifest priority, even if Component::priority differs.
Factories must return resource-free objects with cleanup handled in Shut or the
destructor. A failed configure preserves the existing host. Service currently
binds its listen socket during construction; a second host cannot reuse that
port until the previous host is destroyed. Listener creation failure is reported
by configure and preserves the previous host.

## Component Dependencies

`component.<entry>.depends_on` is a comma-separated list of **manifest entry names**.
Whitespace around entries is trimmed. An empty/whitespace-only list means no
dependencies; leading/trailing commas and empty list members are errors. The raw
string remains in factory params, and AppHost also passes the parsed list in
ComponentConfig::dependencies. A typed AppManifest supplies that vector directly;
its params map does not override the vector. Disabled entries are omitted from
the graph, so requiring one is a missing-dependency error. Only enabled components'
edges participate; this does not auto-enable dependencies.

Code can declare hard prerequisites by **runtime component name**:

```cpp
class Players : public chwell::service::Component {
public:
    std::string name() const override { return "Players"; }
    std::vector<std::string> dependencies() const override { return {"Storage"}; }
    // Init/PostInit may acquire or use Storage after it has passed Init.
};
```

AppHost translates manifest names to the actual names returned by factories and
unions those edges with Component::dependencies(). Repeated edges are coalesced.
Missing/empty targets, self-dependencies, duplicate names and cycles fail before
Init. A cycle reports an actual prerequisite path, e.g.
`Component dependency cycle: Players -> Storage -> Players`; blocked dependents
outside the cycle are excluded. An invalid configure leaves the prepared host
available. Code-only dependency errors are detected after factories return but
before on_register; constructors must remain resource-free.

Service revalidates the entire registered graph after all internal plugins Install,
before the first Init and network start. This includes standalone and plugin-owned
components. `name`, `priority` and `dependencies` are metadata: they must remain
stable, be side-effect-free and must not register components or mutate the Service.
The resolved order is fixed for this run. Plugin Install is still ordered by
plugin priority; component dependencies do not reorder plugin Install/Uninstall or
support lazy plugin construction. Plugins must acquire component prerequisites
during component lifecycle phases, not during Install/on_register.

Every forward phase (Init, PostInit, CheckConfig, PreUpdate, Update and message/
disconnect dispatch) follows the resolved order. Dependencies override priority;
ready nodes use the lowest numeric priority, then stable registration order for
direct Service usage. Without declarations, direct Service preserves its previous
stable-priority order; AppHost preserves priority/entry-name ties. PreShut, Flush
and Shut run in reverse order so consumers can finish before providers flush or
release their resources. This changes the previous forward Flush order.

Startup rollback continues to clean only attempted Init components in reverse
order, including a partially failed Init. Plugin installation rollback may also
clean resources acquired in on_register even when Init never ran. A missing
dependency before network start can be repaired by registering the prerequisite
and retrying; lifecycle operations remain serialized by the application.
`Service::last_error()` and `AppHost::last_error()` expose the dependency/startup
failure; successful startup clears it. Graph validation does not add interface
injection, runtime removal, dependency-aware plugin hot replacement, or concurrency
guarantees for component state.

The platform-free planner is available as `resolve_component_order` from
component_order.h, linked through Chwell::core. It returns input indices without
changing the output on failure. Its iterative cycle walk supports deep graphs
without recursion. Windows can test it independently:

```bash
cmake -S tests/service_order -B build-order
cmake --build build-order --config Debug --parallel 4
ctest --test-dir build-order -C Debug --output-on-failure
```

## Lifecycle and Plugins

- Existing Service::start remains available. New callers should use
  start_checked or AppHost::start to inspect failures.
- Startup automatically installs Service's internal PluginManager. Applications
  using a separate PluginManager must continue managing that instance themselves.
- Registration rejects null/unnamed components and duplicate runtime names.
  Components cannot be added after initialization or while the service is running.
- Plugin registration rejects null/unnamed plugins and duplicate names. Plugin
  installation records ownership for components registered through Service.
- A failed or throwing Install unwinds the attempted plugins in reverse order,
  including the failed plugin, and removes their owned components. An ignored
  component registration failure also fails that plugin's installation.
- Uninstall callbacks run before owned components are removed. Component shutdown
  happens before component destruction; plugins remain alive through this process.
  UninstallAll reports callback failures and continues cleanup. Plugin definitions
  are retained for retry; only installed plugins receive Uninstall.
- Startup failure calls PreShut, then Shut in reverse initialization order for
  every component whose Init was attempted. A failing Init may have allocated
  resources, so it also receives cleanup. Cleanup exceptions do not prevent later
  components from releasing resources.
- Shut must tolerate a partial Init. Plugins must unregister their non-component
  callbacks/timers in Uninstall. Ownership tracking here covers components only;
  it is not a general callback registry or safe dynamic-library hot replacement.
- Lifecycle operations and registration must be serialized by the application.
  Stop must run from the application owner thread, outside network callbacks.
  Legacy shutdown waits for I/O workers before component cleanup and removal.
  Failed component initialization can be retried. After the network has started
  or a network startup has been attempted, construct a new host for another run.

## Redis Migration

RedisConfig::mock_mode defaults to false. DNS, socket, connect, AUTH and SELECT
failures return false, and a transport failure marks the client disconnected.
Connection polling and send/receive use the configured timeouts. DNS resolution
still uses the system blocking resolver and is not bounded by the connect timeout.

For local tests/demos, explicitly set mock_mode = true. The exact environment
value CHWELL_REDIS_MOCK=1 also enables local simulation and emits a warning.
There is no fallback from a failed real connection to memory. Disconnected clients
reject commands, including atomic lock commands. RedisCacheComponent connects in
Init, propagates failure to Service, and disconnects in Shut.

The in-memory mode belongs to one RedisClient instance and is unsuitable for
cross-process caches or locks. The new socket tests cover failure semantics;
they do not replace testing against a real Redis deployment.

## Next Stages

Component ownership, startup rollback, manifest assembly and dependency ordering
are implemented. Discovery, EntitySchema/content and installed reference projects
are described in DISCOVERY.md, ENTITY_SCHEMA.md and PACKAGING.md. Callback tokens,
interface injection, safe dynamic plugin replacement and production reference
deployments remain separate work.

## Validation

The first-stage changes were merged in PR #58 after Linux execution,
Windows Config, ASan/UBSan and TSan CI checks all passed. Local cross-compilation
also verified the focused Service lifecycle, AppHost and Redis tests with
Zig 0.14.1/musl. The subsequent discovery/routing increment is documented in
[DISCOVERY.md](DISCOVERY.md).
