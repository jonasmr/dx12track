# dx12track JSONL format (protocol v3)

The log file produced by `--callstacks` / `Dx12Track_StartLogging` is
[JSON Lines](https://jsonlines.org/): UTF-8 text, one JSON object per line,
`\n` line-terminator, no enclosing array. Each line is a single self-
contained event; there is no top-level document. Consumers stream-parse it
line-by-line.

## Versioning

The first line of every file is a `hello` event whose `protocol` field is
the wire-format version (currently **3**). Consumers should refuse files
whose `protocol` they don't understand; we bump the field whenever an
existing event's fields change shape or are removed. Adding new event
kinds or appending fields to an existing event is a non-breaking change
and does **not** bump `protocol`.

## File-level structure

```
hello                       (always first; one per file)
zero or more events in arrival order:
  module_loaded             (only when callstacks are on)
  module_unloaded
  diag                      (only when --verbose is on)
  created
  renamed
  destroyed
goodbye                      (last; one per file — absent if the host was killed)
```

Order is loose: events from different threads are serialized as they hit
the writer, not by any logical ordering. The only firm constraint is that
`hello` is line 1 and `goodbye` (when present) is the last line.

## Common fields

Every event has these two fields:

| Field | Type | Description |
| --- | --- | --- |
| `event` | string enum | One of the kinds in the table below |
| `ts_ns` | integer | Monotonic time since the DLL attached, in nanoseconds. Resolution = `qpc_freq` from the `hello` event |

## How to write a schema

JSON Lines has no single root schema — each line is its own JSON document.
The conventional approach is one JSON Schema per event kind, dispatched by
the `event` discriminator. The full schema below is a single
[draft 2020-12](https://json-schema.org/) document with a top-level
`oneOf` that branches on `event`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://github.com/jonasmr/dx12track/blob/main/FORMAT.md",
  "title": "dx12track JSONL line",
  "type": "object",
  "required": ["event", "ts_ns"],
  "properties": {
    "event":  { "type": "string" },
    "ts_ns":  { "type": "integer", "minimum": 0 }
  },
  "oneOf": [
    { "$ref": "#/$defs/hello" },
    { "$ref": "#/$defs/created" },
    { "$ref": "#/$defs/renamed" },
    { "$ref": "#/$defs/destroyed" },
    { "$ref": "#/$defs/module_loaded" },
    { "$ref": "#/$defs/module_unloaded" },
    { "$ref": "#/$defs/goodbye" },
    { "$ref": "#/$defs/diag" }
  ],

  "$defs": {
    "HexAddress": { "type": "string", "pattern": "^0x[0-9a-f]+$" },

    "hello": {
      "type": "object",
      "required": ["event", "ts_ns", "pid", "protocol", "qpc_freq", "exe"],
      "properties": {
        "event":    { "const": "hello" },
        "ts_ns":    { "type": "integer", "const": 0 },
        "pid":      { "type": "integer", "minimum": 1 },
        "protocol": { "type": "integer", "const": 3 },
        "qpc_freq": { "type": "integer", "minimum": 1 },
        "exe":      { "type": "string" }
      }
    },

    "created": {
      "type": "object",
      "required": [
        "event","ts_ns","id","type","alloc","heap","dim","format",
        "size","parent_heap_id","name"
      ],
      "properties": {
        "event":          { "const": "created" },
        "id":             { "type": "integer", "minimum": 1 },
        "type":           { "enum": ["Resource","Heap","DescriptorHeap",
                                     "CommandQueue","CommandAllocator",
                                     "CommandList","PipelineState",
                                     "RootSignature","Fence","QueryHeap",
                                     "CommandSignature","Device","Unknown"] },
        "alloc":          { "enum": ["None","Committed","Placed","Reserved","Heap"] },
        "heap":           { "enum": ["None","Default","Upload","Readback",
                                     "Custom","GpuUpload"] },
        "dim":            { "enum": ["Unknown","Buffer","Tex1D","Tex2D","Tex3D"] },
        "format":         { "type": "integer", "minimum": 0 },
        "size":           { "type": "integer", "minimum": 0 },
        "parent_heap_id": { "type": "integer", "minimum": 0 },
        "name":           { "type": "string" },
        "stack": {
          "type": "array",
          "maxItems": 32,
          "items":   { "$ref": "#/$defs/HexAddress" }
        }
      }
    },

    "renamed": {
      "type": "object",
      "required": ["event","ts_ns","id","name"],
      "properties": {
        "event": { "const": "renamed" },
        "id":    { "type": "integer", "minimum": 1 },
        "name":  { "type": "string" }
      }
    },

    "destroyed": {
      "type": "object",
      "required": ["event","ts_ns","id"],
      "properties": {
        "event": { "const": "destroyed" },
        "id":    { "type": "integer", "minimum": 1 }
      }
    },

    "module_loaded": {
      "type": "object",
      "required": ["event","ts_ns","base","size","timestamp",
                   "pdb_age","pdb_guid","name","pdb_name"],
      "properties": {
        "event":     { "const": "module_loaded" },
        "base":      { "$ref": "#/$defs/HexAddress" },
        "size":      { "type": "integer", "minimum": 0 },
        "timestamp": { "type": "integer", "minimum": 0 },
        "pdb_age":   { "type": "integer", "minimum": 0 },
        "pdb_guid":  { "type": "string",
                       "pattern": "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$" },
        "name":      { "type": "string" },
        "pdb_name":  { "type": "string" }
      }
    },

    "module_unloaded": {
      "type": "object",
      "required": ["event","ts_ns","base"],
      "properties": {
        "event": { "const": "module_unloaded" },
        "base":  { "$ref": "#/$defs/HexAddress" }
      }
    },

    "goodbye": {
      "type": "object",
      "required": ["event","ts_ns","exit_code"],
      "properties": {
        "event":     { "const": "goodbye" },
        "exit_code": { "type": "integer", "minimum": 0 }
      }
    },

    "diag": {
      "type": "object",
      "required": ["event","ts_ns","msg"],
      "properties": {
        "event": { "const": "diag" },
        "msg":   { "type": "string" }
      }
    }
  }
}
```

The schema is non-normative: it's a description of what dx12track emits, not
something the producer cross-validates against at runtime.

## Event reference

### `hello`
First line of every file. Identifies the run.

```jsonl
{"event":"hello","ts_ns":0,"pid":50528,"protocol":3,"qpc_freq":10000000,"exe":"D:\\game\\game.exe"}
```

- **`pid`**: process id of the target the DLL was injected into.
- **`protocol`**: the wire-format version. Currently **3**.
- **`qpc_freq`**: ticks per second of `QueryPerformanceCounter`. Divide
  `ts_ns` by `qpc_freq` only if you want raw QPC ticks — `ts_ns` is already
  in nanoseconds.
- **`exe`**: absolute path of the host process at attach time.

### `created`
A D3D12 object was successfully created and registered.

```jsonl
{"event":"created","ts_ns":52606800,"id":12,"type":"Resource","alloc":"Committed","heap":"Default","dim":"Tex2D","format":28,"size":65536,"parent_heap_id":0,"name":"","stack":["0x7ff89ddc9228","0x7ff928dfb972"]}
```

- **`id`**: monotonic, unique across the run. The same id appears in any
  subsequent `renamed` and `destroyed` for the same object.
- **`type`**: high-level D3D12 object class. `Unknown` is reserved and
  should never appear in practice.
- **`alloc`**: how memory was sourced. `None` for non-memory-bearing
  objects (Fence, CommandList, PipelineState, …).
- **`heap`**: heap-property type. `None` when `alloc == "None"`.
- **`dim`**: only meaningful for Resources. `Unknown` for non-resources.
- **`format`**: raw `DXGI_FORMAT` integer. Map via the official table.
- **`size`**: bytes. For buffers this is `D3D12_RESOURCE_DESC.Width`; for
  textures it's `ID3D12Device::GetResourceAllocationInfo().SizeInBytes`;
  for heaps it's `D3D12_HEAP_DESC.SizeInBytes`. `0` for non-memory-bearing
  objects.
- **`parent_heap_id`**: only non-zero for `alloc == "Placed"` — refers to
  the `id` of the heap the resource was placed into. `0` otherwise.
- **`name`**: empty at creation; populated by subsequent `renamed` events
  that mirror the most-recent `ID3D12Object::SetName` /
  `SetPrivateData(WKPDID_D3DDebugObjectName{,W})` call.
- **`stack`** *(optional)*: present only when `--callstacks` /
  `Dx12Track_StartLogging(..., TRUE)` was active. Up to 32 PC addresses as
  hex strings; index 0 is the deepest dx12track-side frame, then the app's
  call site, then up the stack. Resolve with the `module_loaded` events
  (see below).

### `renamed`
A SetName / SetPrivateData(WKPDID_D3DDebugObjectName{,W}) call landed on a
tracked object.

```jsonl
{"event":"renamed","ts_ns":53180300,"id":12,"name":"ShadowMap"}
```

Idempotent: dx12track collapses duplicate names (e.g., when `SetName`
forwards to `SetPrivateData`) into a single event.

### `destroyed`
The last reference on a tracked object's refcount was released.

```jsonl
{"event":"destroyed","ts_ns":99999,"id":12}
```

### `module_loaded` / `module_unloaded`
Emitted only when `--callstacks` is active. At attach time dx12track
enumerates every already-loaded module; thereafter it registers with the
NT loader (`LdrRegisterDllNotification`) to catch every load and unload.

```jsonl
{"event":"module_loaded","ts_ns":0,"base":"0x7ff6c9060000","size":11304960,"timestamp":1780203981,"pdb_age":1,"pdb_guid":"8c3d35b8-e4f6-4d6e-8f7a-fd384f00bdbb","name":"D:\\game\\game.exe","pdb_name":"D:\\game\\game.pdb"}
{"event":"module_unloaded","ts_ns":12345,"base":"0x7ff6c9060000"}
```

- **`base`**, **`size`**: address range. A PC in `[base, base+size)` is
  inside this module.
- **`timestamp`**: `IMAGE_FILE_HEADER.TimeDateStamp`. Combine with `size`
  to fetch the binary itself from a symbol server (image hash).
- **`pdb_guid`**, **`pdb_age`**: identifiers for the matching PDB. Fetch
  via `https://msdl.microsoft.com/download/symbols` (or your own symstore)
  at the path
  `<pdb_filename>/<pdb_guid_with_dashes_removed><pdb_age>/<pdb_filename>`.
- **`name`**: full module path as loaded.
- **`pdb_name`**: PDB path baked into the PE's debug directory at link
  time.

A PC in a `stack` array resolves to `module_name + (pc - module_base)`
inside the matching PDB. dx12track itself never symbolicates — the JSONL
is enough to do it offline.

### `goodbye`
Last line of a normal run. Absent when the host was force-killed.

```jsonl
{"event":"goodbye","ts_ns":1234567890,"exit_code":0}
```

### `diag`
Verbose-mode trace events for diagnosing injection/hook-install issues.
Only present when `--verbose` was on. Stable shape, but the `msg` content
is freeform English text and not parser-friendly.

```jsonl
{"event":"diag","ts_ns":48200,"msg":"InstallExportHooks: D3D12CreateDevice export at 0x7ff..."}
```

## Notes for consumers

- **Treat addresses as opaque strings.** `base`, `module_unloaded.base`,
  and entries inside `stack` are quoted hex (`"0x..."`) to preserve all 64
  bits through JSON parsers that decode numbers as IEEE 754 doubles.
- **String escaping** follows standard JSON: `\\`, `\"`, `\n`, `\r`,
  `\t`, and `\u00XX` for other control chars. UTF-16 wide-char names are
  encoded as UTF-8 in the JSON string.
- **No size cap on the file.** A long-running session can produce gigabytes
  of events; consume the file as a stream.
- **Repeated `name`s are normal.** Two different objects often share a
  debug name. Use `id` as the unique key.
- **Negative timestamps don't exist.** `ts_ns` is monotonic from 0 at
  attach time. If you see decreases, the file was truncated or interleaved
  with another run.
- **Robustness.** A consumer that skips unknown event kinds and ignores
  unknown fields on known events will keep working across non-breaking
  format additions.
