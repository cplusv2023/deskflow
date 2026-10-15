**Deskflow** is a free and open source keyboard and mouse sharing app.
Use the keyboard, mouse, or trackpad of one computer to control nearby computers,
and work seamlessly between them.

Deskflow acts as a software KVM (without video) that allows you to:
- Share keyboard and mouse input across multiple computers
- Synchronize clipboard content between machines
- Work seamlessly across different operating systems (Windows, macOS, Linux, BSD)

Deskflow software consists of a **server** (primary computer) that shares its input devices and **clients** (secondary computers) that receive and execute the input commands over a TCP network connection.

### Architecture Overview

Deskflow is built with a modular, cross-platform architecture:
Note: In older versions of deskflow you may see the term `Screen` used in place of `Computer`
```
┌─────────────────┐    Network Protocol    ┌─────────────────┐
│   Server App    │◄──────────────────────►│  Client App     │
│                 │     (Port 24800)       │   (Windows)     │
│ ┌─────────────┐ │                        │ ┌─────────────┐ │
│ │  Computer   │ │                        │ │  Computer   │ │
│ │  Platform   │ │                        │ │  Platform   │ │
│ │   Layer     │ │                        │ │   Layer     │ │
│ └─────────────┘ │                        │ └─────────────┘ │
└─────────────────┘                        └─────────────────┘
┌───────┐ ┌───────┐
│ Keyb. │ │ Mouse │
└───────┘ └───────┘

                                           ┌─────────────────┐
                                           │  Client App     │
                                           │    (macOS)      │
                                           │ ┌─────────────┐ │
                                           │ │  Computer   │ │
                                           │ │  Platform   │ │
                                           │ │   Layer     │ │
                                           │ └─────────────┘ │
                                           └─────────────────┘

                                           ┌─────────────────┐
                                           │  Client App     │
                                           │   (Custom)      │
                                           │ ┌─────────────┐ │
                                           │ │  Computer   │ │
                                           │ │  Platform   │ │
                                           │ │   Layer     │ │
                                           │ └─────────────┘ │
                                           └─────────────────┘
```

### More info

For more info, see our [Wiki](https://github.com/deskflow/deskflow/wiki).

Check out our [Building guide](build.md) or our general @ref contributing_guide "Contributing section". We also have a detailed [Protocol Reference](protocol_reference.md).

For the proposed clipboard extension, see the [on-demand clipboard goals and architecture](clipboard-design.md) and [Ubuntu prototype progress](clipboard-progress.md) (Chinese).

The [macOS implementation and validation record](clipboard-macos-progress.md) tracks the `macos` branch, including its Finder and integration gates.

Cross-platform semantics and shared fixtures are described in the [clipboard interface contract draft](clipboard-contract.md) (Chinese).

The next core integration steps and their acceptance gates are tracked in the [clipboard M5 integration plan](clipboard-m5-integration.md) (Chinese).

- [内嵌剪贴板首版：构建、使用与冻结协议](clipboard-core-v1.md)
