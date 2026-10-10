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
component.Players.max_players = 10000
```

The configuration entry name selects a registered factory. Factories return
components with their existing runtime names, which may differ from the entry
name. Both manifest entry names and runtime component names must be unique.
Disabled entries need no factory and are never constructed. Component parameters
are passed to the factory unchanged; each factory owns its parameter validation.
Port/thread fields, enabled flags and priorities are parsed strictly. Port zero
is allowed for ephemeral test listeners; thread counts must be positive, except
logic_workers may be zero to select the reactor count.

AppHost validates all entries and constructs all components before registration.
Enabled components are registered by priority, then entry name. Service uses the
manifest priority for lifecycle ordering, even if Component::priority differs.
Factories must return resource-free objects with cleanup handled in Shut or the
destructor. A failed configure preserves the existing host. Service currently
binds its listen socket during construction; a second host cannot reuse that
port until the previous host is destroyed. Listener creation failure is reported
by configure and preserves the previous host.

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

The first increment covers component ownership, startup rollback and builtin
manifest assembly. Dependency declarations/topological ordering, callback tokens,
versioned dynamic plugins, cross-process discovery, EntitySchema and production
reference deployments remain separate work in the recommended order.

## Validation

The Windows Config regression suite passes. The focused Service lifecycle,
AppHost and Redis tests compile and link for x86_64 Linux with Zig 0.14.1/musl.
This Windows workspace has no Linux runtime, so those tests have not been run
here; Linux execution and sanitizer checks are still required before merging.
