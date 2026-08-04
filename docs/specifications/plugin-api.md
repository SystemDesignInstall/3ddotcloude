# Plugin and Capability API Specification

Status: Draft
References: ADR-013 (plugin architecture), ADR-034 (capability negotiation), ADR-021 (mock adapters for tests), ADR-004 (Capability API is Constitution-protected)

This document defines the Spatial Platform plugin architecture: how algorithms plug into the platform, how capabilities are declared and negotiated, and how adapters are isolated from Core. It pairs with `worker-protocol.md`, which defines how an adapter's algorithm actually executes.

## 1. Layering

The runtime is strictly layered:

```
Core → PluginManager → Plugin → Adapter → Algorithm
```

- **Core** owns project storage, scheduling, provenance, and the capability registry.
- **PluginManager** loads plugins, validates their descriptors, and answers capability queries.
- **Plugin** is a loadable unit that registers one or more adapters.
- **Adapter** is the platform-facing interface an algorithm implements.
- **Algorithm** is the worker-side implementation of a capability.

Plugins never bypass Core: every interaction with projects, artifacts, cache, or the scheduler goes through Core APIs. A plugin that bypasses Core (e.g. by writing directly into `artifacts/`) is rejected at load time by descriptor validation and is a contract violation.

## 2. ProcessingAdapter interface

Every adapter implements the `ProcessingAdapter` interface. Sketch signatures in C++:

```cpp
class ProcessingAdapter {
public:
  virtual AdapterDescriptor descriptor() const = 0;
  virtual EnvironmentValidationResult validate_environment(const Environment&) const = 0;
  virtual std::vector<PlanStep> create_plan(const AdapterRequest&) const = 0;
  virtual TaskContext execute(TaskContext) = 0;
};
```

- `descriptor()` returns the adapter's static identity and declarations (Section 3).
- `validate_environment(env)` checks executables, OS, and GPU backend before the adapter is ever offered work; it gates availability (Section 5).
- `create_plan(AdapterRequest)` turns a request (inputs, configuration, output schema) into a plan of `PlanStep`s. Planning never runs the algorithm; it is pure reasoning over inputs.
- `execute(TaskContext)` runs the algorithm. In production it is invoked inside a worker process under the worker protocol; in tests it may be invoked in-process via a mock harness.

## 3. AdapterDescriptor fields

`descriptor()` returns exactly these fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `id` | string | Stable machine-readable adapter id. |
| `name` | string | Human-readable name. |
| `version` | string | Adapter version. |
| `supported_capabilities` | string[] | Capabilities this adapter provides (Section 4). |
| `required_executables` | string[] | External binaries the algorithm needs. |
| `supported_os` | string[] | OS identifiers, e.g. `windows`, `linux`. |
| `supported_gpu_backend` | string[] | e.g. `cuda`, `vulkan`, `none`. |
| `input_schemas` | object[] | Schema ids and artifact types the adapter accepts. |
| `output_schemas` | object[] | Schema ids and artifact types the adapter produces. |
| `license_reference` | string | Pointer to the adapter's license text. |

The descriptor is loaded, validated, and cached by `PluginManager`; any field that fails validation rejects the whole plugin.

## 4. Capability taxonomy

Capabilities are the platform's vocabulary for "what an algorithm can do." The built-in taxonomy:

- `SparseReconstruction`
- `DenseStereo`
- `BundleAdjustment`
- `ICP`
- `SurfaceReconstruction`
- `Texturing`
- `GaussianGeneration`
- `LidarOdometry`
- `LoopClosure`
- `GnssIntegration`

**Extensibility rule:** the Capability API is protected by the Constitution (ADR-004). Adding a new capability is an RFC-track change, not a plugin change. Plugins may declare support for built-in capabilities only; declaring an undeclared capability is a validation error. This keeps the registry small, auditable, and interoperable across the ecosystem.

## 5. Negotiation

Capability negotiation happens at load time and at plan time:

1. The engine asks `PluginManager` for plugins by capability: `plugins(capability = "DenseStereo")`.
2. `PluginManager` returns only adapters whose descriptor lists the capability.
3. Availability is gated by environment validation: an adapter whose `required_executables`, OS, or GPU backend are unsatisfied is filtered out by `validate_environment` (the "doctor"). Missing executables produce a diagnostic, never a hard failure at dispatch.
4. Among available adapters, the engine selects by declared priority and input/output schema match, then builds the plan via `create_plan`.
5. The same negotiation runs on the worker side so that only capabilities the worker's environment supports are ever dispatched.

## 6. Isolation

- **Process isolation:** adapters never execute in the Core process. In production, execution happens in separate worker processes speaking the worker protocol (ADR-011). A crashing algorithm cannot corrupt Core or another adapter.
- **Mock adapters:** for tests, adapters may be replaced with in-process mock implementations (ADR-021). Mocks implement the full `ProcessingAdapter` interface, declare real capabilities, and honor the worker protocol's message flow; only the algorithm itself is faked. This lets the scheduler, provenance, and artifact store be tested end-to-end without external executables.

## 7. M0: interfaces + mock plugins

The M0 milestone ships:

- The `ProcessingAdapter` interface and `AdapterDescriptor` type.
- A `PluginManager` that loads plugin descriptors from a directory, validates them, and answers capability queries.
- Mock plugins implementing `SparseReconstruction` and `DenseStereo` with fake `execute` bodies (in-process, no external executables).
- Full doctor environment validation against a real environment object.
- **Deferred:** dynamic loading of shared libraries (`.so`/`.dll`). M0 loads plugins from declarative JSON plus in-process mocks; binary plugin loading arrives in a later milestone once the descriptor contract is stable.
