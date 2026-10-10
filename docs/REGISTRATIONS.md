# Interfaces and owned registrations

Service provides explicit interface bindings and cancellation ownership. These
facilities are independently implemented; ARK was an architectural reference.

## Interface bindings

An interface must be polymorphic. Its provider must also be a Component already
owned by the Service. Register it in `on_register`, before initialization:

```cpp
struct ContentReader {
    virtual ~ContentReader() = default;
    virtual int item_count() const = 0;
};
class Content : public chwell::service::Component, public ContentReader {
public:
    std::string name() const override { return "Content"; }
    int item_count() const override { return 42; }
    void on_register(chwell::service::Service& service) override {
        if (!service.register_interface<ContentReader>(*this))
            throw std::runtime_error("ContentReader registration failed");
    }
};
// In the consumer's Init/PostInit:
auto* content = service.get_interface<ContentReader>();
```

Lookup uses the explicitly registered type, without scanning components or
implicitly binding concrete/base types. One provider per interface is allowed;
duplicates, incompatible providers and foreign owners are rejected. A rejected
registration inside `on_register` rejects that component even if the component
ignores the return value. `get_component<T>()` retains its existing behavior.

Bindings stay available through component Shut and plugin Uninstall, and are
removed before provider destruction. Ordinary components retained by a stopped
Service retain their bindings, but their resources may already be shut down.
Returned pointers are borrowed: do not cache them beyond provider removal.
Consumers must still declare component dependencies for startup/shutdown order;
binding an interface does not create a dependency or perform automatic injection.

## Cancellation tokens

`core::Registration` is move-only. `reset()` or destruction cancels once;
move assignment first cancels the previous registration. Retain the token or
transfer it to Service; discarding a temporary cancels immediately.

| Source | Scoped API | Existing API |
| --- | --- | --- |
| EventBus | `subscribe_scoped<EventT>(callback, priority)` | `subscribe/unsubscribe` |
| TimerWheel / TimerManager | `add_timer_scoped(delay_ms, callback)`, `add_repeat_timer_scoped(interval_ms, callback)` | TimerHandle methods |
| ProtocolRouterComponent | `register_handler_scoped(cmd, callback)` | `register_handler` |

Empty callbacks and non-positive scoped timer delays/intervals return empty
tokens. Scoped protocol registration rejects a cmd collision. Legacy handler
registration continues to replace the cmd handler; cancelling the old scoped
token cannot erase that replacement.

```cpp
// Inside Component::Init, after saving Service* in on_register:
return service_->track_registration(*this,
    chwell::event::EventBus::instance().subscribe_scoped<MyEvent>(
        [this](const MyEvent& event) { handle(event); }));
```

`track_registration(component, token)` requires an owner in that Service. The
owner-free `track_registration(token)` is available only during plugin Install,
and associates the token with that plugin. Invalid owners/tokens or registration
during shutdown return false and immediately cancel the supplied token.

Service cancels all tracked registrations before its first PreShut callback,
including startup rollback and never-started destruction. Plugin uninstall first
cancels its own and its components' tracked registrations, then shuts down the
components and invokes Uninstall. An `on_register` exception/rejection cancels
that component's tracked registrations before cleanup and destruction.
Registrations within an owner group are cancelled in reverse acquisition order;
all-owner cleanup visits groups in reverse first-acquisition order. Cancellation
exceptions are contained so the other registrations still get cleaned up.

For retryable initialization, acquire runtime callbacks in Init/PreUpdate on
every attempt. Callbacks acquired in `on_register` are cancelled on rollback and
are not automatically re-created for retained components. Plugins reinstall
their registrations on a subsequent installation attempt. FrameSync's timeout
timer now uses a scoped token and waits for its callback in Shut/destruction.

## Thread and lifetime boundaries

Service lifecycle, registration ownership, interface registration and lookup
belong to the application owner thread. Tokens may be reset from a different
thread than dispatch, but the same token object must not be accessed concurrently.
RegistrationGroup itself is an owner-thread container.

Reset first disables the callback and waits for an invocation executing on another
thread. A copied event/handler dispatch wrapper or an extracted timer task cannot
start the user callback after reset returns. A repeating timer cancelled during
dispatch cannot re-arm. Tokens can outlive their source and still be safely reset;
normal dispatch threads must be stopped before destroying the source itself.

Self-cancellation is supported: the current invocation finishes and nested/future
dispatches are suppressed. Callbacks for the same scoped registration are
serialized; recursive dispatch on its executing thread remains supported. This
does not make the callback's other shared state thread-safe.

Do not stop a Service, uninstall a plugin, destroy a timer source, or synchronously
cancel callbacks in mutually dependent threads from inside a callback. Waiting
for each other's callback locks can deadlock. Request lifecycle changes on the
owner thread after callbacks return, and avoid holding business locks while
waiting for cancellation.

Legacy registration calls are not automatically owned. Legacy unsubscribe/clear
or handler replacement alone do not provide the scoped reset's waiting guarantee.
Subscriptions owned by SchemaSyncRoom, network callback setters and custom sources
are outside this token API and retain their existing contracts. Tokens do not
cancel arbitrary work already posted by a callback.

This increment does not guarantee safe dynamic-library unload/hot replacement:
threads, queued tasks, virtual objects, borrowed interface pointers and other
references to plugin code still require a separate unload protocol.

## Validation and example

Portable tests cover token moves, reverse cleanup, callback capture release,
dispatch-snapshot cancellation, self-cancellation, waiting for in-flight events
and timers, timer re-arm cancellation and exact-type/multiple-inheritance lookup.
Linux tests additionally exercise protocol routing and Service/plugin rollback,
retry, interface removal and destructor cleanup. Windows CI runs the portable
subset; the complete network framework still requires Linux/POSIX.

The installed [reference service](../examples/reference_service/README.md) uses
ContentReader and an owned event subscription. Its smoke run checks that events
stop before PreShut and prerequisites remain available during consumer Shut.
