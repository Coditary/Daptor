# Examples

Sample programs and config templates for **Daptor**. Nothing here is required at runtime — copy config into `~/.config/tui-debug/` if you want custom defaults.

## Quick try

```bash
./cpp/build/tui-debug-ui examples/python/hello.py
```

## Layout

| Path | Purpose |
|------|---------|
| `python/` | Python debug targets (default: `hello.py`) |
| `native/` | C/C++ sources; run `native/build.sh` to compile |
| `config/` | Example `config.yaml`, `theme.json`, compose templates |
| `workspaces/` | Mini projects (e.g. file-tree picker demo) |

## Python programs

| File | Use case |
|------|----------|
| `hello.py` | Default smoke test, locals scrolling, breakpoints |
| `step_in_demo.py` | Step-in / step-out |
| `stdlib_step.py` | Step into Python stdlib (integration tests) |
| `interactive_demo.py` | stdin-driven Python session |
| `network_demo.py` | Network panel traffic |
| `runtime_source_demo.py` | Runtime source panel |
| `multi_file.py` + `greeter.py` | Multi-file navigation |

## Native programs

```bash
./examples/native/build.sh
./cpp/build/tui-debug-ui examples/native/reverse_demo
./cpp/build/tui-debug-ui --rr examples/native/reverse_demo
./cpp/build/tui-debug-ui examples/native/exception_demo 2
```

Built binaries (`reverse_demo`, etc.) are gitignored — compile locally with `native/build.sh`.

## Config templates

```bash
mkdir -p ~/.config/tui-debug
cp examples/config/config.example.yaml ~/.config/tui-debug/config.yaml
cp examples/config/theme.example.json ~/.config/tui-debug/theme.json
```
