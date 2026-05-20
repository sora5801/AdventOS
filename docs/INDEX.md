# AdventOS docs index

171 docs across 171 sessions of work, grouped by topic.  Doc numbers
are filename order (mostly chronological); session numbers in
parentheses are the actual session each doc records.  When the two
diverge it's because several paths were worked on in parallel during
the same session.

If you're new to the codebase, read in this order:
1. The kernel foundation chain (`01` → `08`).
2. The userspace + Unix surface chain (`09` → `30`).
3. Whichever **Path** you care about (A/B/C/D/E — see below).

Reference docs (not session logs): [`agent-api.md`](agent-api.md),
[`agent-cookbook.md`](agent-cookbook.md).

---

## Kernel foundations (sessions 1–8)

The bare-metal substrate: bootloader → 32-bit kernel → memory →
multitasking → userspace → ATA → AdventFS → ELF.

| Doc | Topic |
|---|---|
| [01](01-bootloader-and-kernel.md) | Bootloader + 32-bit kernel from scratch |
| [02](02-bump-kmalloc-and-e820.md) | Bump kmalloc + BIOS E820 + meminfo |
| [03](03-pmm-and-paging.md) | Physical Memory Manager + 4 KB paging |
| [04](04-multitasking.md) | Round-robin scheduler + preemptive context switch |
| [05](05-userspace-and-syscalls.md) | TSS, ring 3, syscalls, per-process page directories |
| [06](06-allocator-ata-sync-rtc.md) | Free-list kmalloc + ATA + sync primitives + RTC |
| [07](07-finish-userspace.md) | Finishing the userspace path |
| [08](08-fs-and-elf-loader.md) | AdventFS + ELF32 loader |

## Userspace + Unix surface (sessions 9–30)

The Unix-shaped middle: shell, syscalls, signals, jobs, networking,
filesystems, /proc, multi-connection TCP.

| Doc | Topic |
|---|---|
| [09](09-user-c-runtime.md) | Real userspace runtime (writing user programs in C) |
| [10](10-networking.md) | Networking: PCI + RTL8139 + ARP/IPv4/ICMP |
| [11](11-tcp-http-usershell.md) | Minimum-viable TCP + HTTP server + ring-3 shell |
| [12](12-argv-and-fds.md) | argv, per-process fds, real `cat` and `echo` |
| [13](13-sockets.md) | Sockets API + userspace HTTP server |
| [14](14-fork-exec-wait.md) | fork, exec, wait, real userspace shell |
| [15](15-pipes-and-redirection.md) | Pipes, redirection, libuser stdout |
| [16](16-signals.md) | Full Unix signals |
| [17](17-user-malloc.md) | User-mode malloc + SYS_BRK |
| [18](18-tty-raw-mode.md) | TTY layer with cooked/raw input modes |
| [19](19-ed-and-fs-write.md) | `ed` line editor, AdventFS write, .bss fix |
| [20](20-job-control.md) | Job control: process groups, sessions, SIGSTOP/SIGCONT |
| [21](21-udp-dhcp-dns.md) | UDP, DHCP, DNS + the bootloader sector-count gotcha |
| [22](22-init-system.md) | Real init system + orphan reparenting |
| [23](23-fs-bitmap.md) | Free-sector bitmap for AdventFS |
| [24](24-mmap-and-lazy-paging.md) | mmap on a fd + page-fault handler grows up |
| [25](25-hierarchical-fs.md) | Hierarchical filesystem with directories |
| [26](26-coreutils-sweep.md) | Userspace coreutils sweep |
| [27](27-block-cache.md) | Block cache + write-back filesystem |
| [28](28-vfs-and-procfs.md) | VFS abstraction + /proc filesystem |
| [29](29-network-apps.md) | Userspace network apps: nc / wget / telnet / IRC |
| [30](30-multi-conn-tcp.md) | Real multi-connection TCP |

## SMP, graphics, audio, USB (sessions 31–43)

Second CPU, framebuffer console, sound, USB stack, mass storage,
crypto bootstrap.

| Doc | Topic |
|---|---|
| [31](31-smp.md) | SMP: bringing up application processors |
| [32](32-vbe-fbcon.md) | VBE/VESA graphics + framebuffer console |
| [33](33-ap-scheduling.md) | AP scheduling: putting the second CPU to work |
| [35](35-libc-dynamic-linking.md) | Real C library + dynamic linking |
| [36](36-tls13-https.md) | TLS 1.3 + HTTPS |
| [37](37-ac97-pcm.md) | AC97 sound + PCM playback |
| [38](38-smp-user-tasks.md) | SMP for user tasks (BKL + race fixes) |
| [39](39-x509-curl-interop.md) | X.509 + curl interop |
| [40](40-usb-uhci-hid.md) | USB stack: UHCI + core + HID keyboard |
| [41](41-usb-mass-storage.md) | USB Mass Storage + block-device abstraction |
| [42](42-fs-multi-instance-mount.md) | Multi-instance AdventFS + USB mounted at /mnt/usb |
| [43](43-ecdsa-p256.md) | ECDSA-P256 (closing the Schannel-compat gap) |

## Real-world interop, editor, ssh, debugger (sessions 45–60)

vi, login, ssh server + client, ptrace, RSA, cert chains, NTP/DHCP/DNS
real-world coverage.

| Doc | Topic |
|---|---|
| [45](45-real-world-https-get.md) | Real-world HTTPS GET |
| [46](46-vi-modal-editor.md) | vi, a modal text editor |
| [47](47-login-and-multi-user.md) | Login + multi-user |
| [48](48-permission-enforcement.md) | Permission enforcement |
| [49](49-shell-polish.md) | Shell polish |
| [50](50-ssh-server.md) | SSH server (TLS-backed remote shell) |
| [51](51-ssh-rfc-4253.md) | SSH-2 (RFC 4253) wire protocol |
| [52](52-pty-pairs.md) | Pseudo-terminal pairs |
| [53](53-pubkey-auth.md) | Public-key authentication (RFC 4252 §7) |
| [54](54-host-key-persistence.md) | Host-key persistence on disk |
| [55](55-real-world-tls-interop.md) | Real-world TLS interop |
| [56](56-pty-signals-and-ssh-rekey.md) | PTY-driven signals + SSH rekey + ext-info |
| [57](57-ptrace-debugger.md) | ptrace debugger |
| [58](58-rsa-sign-verify.md) | RSA-PKCS#1 v1.5 sign and verify |
| [59](59-cert-chain-validation.md) | X.509 cert chain validation against CA root store |
| [60](60-dns-dhcp-ntp.md) | Real DNS + DHCP + NTP |

## Agent + JSON-RPC + structured pipelines (sessions 64–69, 80)

The agentd JSON-RPC daemon, MCP server, structured `|>` pipelines, SMP
race fixes that landed alongside the agent work.

| Doc | Topic |
|---|---|
| [64](64-agent-rpc.md) | Agent JSON-RPC tooling layer |
| [65](65-mcp-server.md) | MCP server support in agentd |
| [66](66-smp-loopback-fix.md) | SMP TCP-loopback race fix |
| [67](67-serial-keyboard-input.md) | Serial keyboard input |
| [68](68-smp2-deadlock-fixes.md) | SMP=2 deadlock fixes |
| [69](69-structured-pipelines.md) | Structured pipelines (JSONL convention) |

## Path A — usable Unix surface (sessions 83–86)

bash-compat shell polish: mid-line editing, coreutils gap-fill,
man pages, vi polish.

| Doc | Topic |
|---|---|
| [70](70-usable-unix-coreutils.md) | Coreutils gap-fill + selftest reliability |
| [71](71-shell-mid-line-editing.md) | Shell mid-line editing |
| [72](72-man-pages.md) | Man pages |
| [73](73-vi-polish.md) | vi polish |

## Path D — Lua interpreter (sessions 87–89)

Single-file Lua-subset interpreter from scratch (≈1100 lines, ≈28 KB).

| Doc | Topic |
|---|---|
| [74](74-tinylua.md) | Path D begins: Lua-syntax interpreter from scratch |
| [75](75-lua-error-handling-and-gc.md) | Error handling, closures, GC |
| [76](76-lua-multireturn.md) | Multi-return + generic for |

## Path B — cc compiler (sessions 90–135)

A C-subset compiler from scratch, then a real tcc port.

### `cc` (subset compiler, sessions 90–106)

| Doc | Topic |
|---|---|
| [77](77-tinycc.md) | Phase 1: foundation |
| [78](78-cc-strings.md) | Phase 2.1: string literals + `puts` |
| [79](79-cc-chars-pointers.md) | Phase 2.2: char, pointers, & / *, arrays |
| [80](80-cc-globals.md) | Phase 2.3: global variables |
| [81](81-cc-printf.md) | Phase 2.4: `printf` |
| [82](82-cc-preprocessor.md) | Phase 2.5: the preprocessor |
| [83](83-cc-compound-ops.md) | Phase 2.6: compound operators |
| [84](84-cc-structs.md) | Phase 3.1: structs |
| [85](85-cc-function-pointers.md) | Phase 3.2: function pointers |
| [86](86-cc-sizeof-and-scaled-pointers.md) | Phase 3.3: `sizeof` + scaled pointer arith |
| [87](87-cc-multi-file.md) | Phase 3.4: multi-file compilation |
| [88](88-cc-struct-value-assign.md) | Phase 3.5: struct value assignment |
| [89](89-cc-array-of-struct.md) | Phase 3.6: array-of-struct + indexed member access |
| [90](90-cc-enums.md) | Phase 3.7: enums |
| [91](91-cc-typedef.md) | Phase 3.8: typedef |
| [92](92-cc-variadics.md) | Phase 3.9: real variadic functions |
| [93](93-cc-struct-by-value.md) | Phase 3.10: struct-by-value function args |
| [108](108-pathB-capstone.md) | Phase 4 capstone: SBV returns, static/extern, fp typedef |
| [112](112-pathB-optimizations.md) | Optimization passes (register alloc, constant fold, peephole, DCE) |
| [115](115-pathB-language-corners.md) | 11 language-corner features in one branch |

### `tcc` (real tcc port, sessions 133–137)

| Doc | Topic |
|---|---|
| [119](119-pathB-tcc-foundation.md) | tcc port, Phase 1 foundation |
| [120](120-pathB-tcc-phase2.md) | tcc port, Phase 2 (running inside AdventOS) |
| [121](121-pathB-tcc-fully-working.md) | tcc fully works inside AdventOS |
| [122](122-pathB-tcc-ux-polish.md) | tcc UX polish |

## Path E — drivers (sessions 118–127)

virtio family, e1000, AHCI, NVMe, EHCI, CDC-ECM, virtio-9p.

| Doc | Topic |
|---|---|
| [105](105-pathE-drivers.md) | virtio-blk / virtio-net / CDC-ACM / aplay |
| [106](106-pathE-more-virtio.md) | virtio-rng / virtio-console / virtio-balloon |
| [107](107-pathE-9p.md) | WSL build + virtio-9p host filesystem |
| [108](108-pathE-9p-write-and-irq.md) | 9p writes + IRQ-driven virtio |
| [109](109-pathE-e1000-and-rename.md) | e1000 NIC + 9p atomic rename |
| [110](110-pathE-ahci.md) | AHCI SATA controller |
| [122](122-pathE-vscsi-cdc-tty.md) | virtio-scsi + CDC-ACM TTY |
| [126](126-pathE-nvme-ehci-ahci-ecm.md) | NVMe + EHCI + AHCI IRQ/NCQ + CDC-ECM |
| [127](127-pathE-ehci-transfers.md) | EHCI transfer integration |

## Path C — graphics + window manager (sessions 107–171)

The largest path: framebuffer → libgfx → wmd compositor → ten+ apps
(wmterm, wmedit, wmcalc, wmpaint, wmview, wmfiles, wmsysinfo, wmps,
wmclock, …).

### Foundation (sessions 107–124)

| Doc | Topic |
|---|---|
| [94](94-pathC-fb.md) | Phase 1: userspace framebuffer access |
| [95](95-pathC-libgfx.md) | Phase 2: libgfx (software drawing library) |
| [96](96-pathC-mouse.md) | Phase 3: PS/2 mouse driver |
| [97](97-pathC-doublebuffer.md) | Phase 4: double-buffered libgfx |
| [98](98-pathC-wm.md) | Phase 5: window manager daemon |
| [99](99-pathC-wmprotocol.md) | Phase 6: WM client protocol |
| [100](100-pathC-input.md) | Phase 7: input routing to focused clients |
| [101](101-pathC-keyboard.md) | Phase 8: keyboard input + wmtype demo |
| [102](102-pathC-apps.md) | Phase 9: wmclock + wmpaint |
| [103](103-pathC-close.md) | Phase 10: close buttons + WM_EV_CLOSE |
| [104](104-pathC-hover-focus.md) | Phase 11: hover vs focus separation |
| [105](105-pathC-taskbar.md) | Phase 12: taskbar |
| [106](106-pathC-launcher.md) | Phase 13: in-WM app launcher |
| [107](107-pathC-fonts.md) | Phase 14: scalable fonts |
| [108](108-pathC-taskbar-clock.md) | Phase 15: taskbar system clock |
| [109](109-pathC-multiwindow.md) | Phase 16: multi-window per client |
| [110](110-pathC-clean-mode.md) | Phase 17: --clean mode + wmpair |
| [111](111-pathC-ctxmenu.md) | Phase 18: focus label + right-click menu |

### Apps and ergonomics (sessions 125–146)

| Doc | Topic |
|---|---|
| [112](112-pathC-gc-attempt.md) | Phase 19: gc-sections attempt + smoke fix |
| [113](113-pathC-fb-mmio-guard.md) | Phase 19: VBE FB pages stay out of PMM |
| [114](114-pathC-wallpaper.md) | Phase 20: procedural desktop wallpaper |
| [115](115-pathC-wmfiles.md) | Phase 21: wmfiles file manager |
| [116](116-pathC-wmsysinfo.md) | Phase 22: wmsysinfo dashboard |
| [117](117-pathC-wmps.md) | Phase 23: wmps process viewer |
| [117](117-pathC-resize.md) | Phase 24: window resize via drag handle |
| [118](118-pathC-user-gc-sections.md) | Phase 25: user-program --gc-sections |
| [119](119-pathC-minmax.md) | Phase 26: maximize + minimize buttons |
| [120](120-pathC-wmterm.md) | Phase 27: wmterm terminal emulator |
| [121](121-pathC-alttab.md) | Phase 28: Alt-Tab cycling |
| [122](122-pathC-clipboard.md) | Phase 29: clipboard |
| [123](123-pathC-wmedit.md) | Phase 30: wmedit text editor |
| [124](124-pathC-snap-shadows.md) | Phase 31: snap-to-edge + drop shadows |
| [125](125-pathC-wmcalc.md) | Phase 32: wmcalc calculator |
| [126](126-pathC-caret-selection.md) | Phase 33: wmedit caret + drag selection |
| [127](127-pathC-usbtablet.md) | Phase 34: USB tablet absolute pointer |
| [128](128-pathC-cursor-pst-shell.md) | Phase 35: cursor cleanup + PST clock + Shell launcher |
| [129](129-pathC-notifications.md) | Phase 36: toast notifications |
| [130](130-pathC-mouse-hotspot.md) | Phase 37: mouse hotspot compensation |
| [131](131-pathC-shell-pst-marker.md) | Phase 38: launcher cleanup + PST taskbar |
| [132](132-pathC-resize-edges.md) | Phase 39: resize from any edge / corner |

### Workspaces, apps continued (sessions 147–156)

| Doc | Topic |
|---|---|
| [133](133-pathC-workspaces.md) | Phase 40: workspaces / virtual desktops |
| [134](134-pathC-move-to-workspace.md) | Phase 41: move window to workspace |
| [135](135-pathC-wmview.md) | Phase 42: wmview image viewer |
| [136](136-pathC-wmpaint-save.md) | Phase 43: wmpaint Ctrl-S save to PPM |
| [137](137-pathC-screenshot.md) | Phase 44: Alt+P screenshot |
| [138](138-pathC-wmedit-search.md) | Phase 45: wmedit Ctrl-F incremental search |
| [139](139-pathC-wmedit-undo.md) | Phase 46: wmedit Ctrl-Z undo |
| [140](140-pathC-wmedit-redo.md) | Phase 47: wmedit Ctrl-Y redo |
| [141](141-pathC-snap-preview.md) | Phase 48: snap-to-edge ghost preview |
| [142](142-pathC-wmcalc-decimal-memory.md) | Phase 49: wmcalc decimal + memory keys |

### Terminal emulator polish (sessions 157–168)

| Doc | Topic |
|---|---|
| [143](143-pathC-wmterm-pty-nonblock.md) | wmterm input + close, fixed |
| [144](144-pathC-wmterm-sighup.md) | SIGHUP on PTY master close |
| [145](145-pathC-wmterm-scrollback.md) | wmterm scrollback (PgUp/PgDn) |
| [146](146-pathC-kbd-grab.md) | Keyboard grab |
| [147](147-pathC-wmterm-interactive-polish.md) | Interactive polish (backspace + PgUp + CSI K) |
| [148](148-pathC-wmterm-geom-cursor.md) | Geometry + wmd cursor sprite |
| [149](149-pathC-wmterm-clear-wheel.md) | `clear` + mouse-wheel scrollback |
| [150](150-pathC-resize-cursors.md) | Resize-zone-aware cursor sprites |
| [151](151-pathC-wmterm-select-copy.md) | Text selection + clipboard |
| [152](152-pathC-wmterm-ansi-color.md) | ANSI color support |
| [153](153-pathC-polish-bundle.md) | Five polish items in one session |
| [154](154-pathC-polish-bundle-2.md) | Four more polish items |

### Stability + cleanup (sessions 169–171)

| Doc | Topic |
|---|---|
| [155](155-pathC-stability-flake-hunt.md) | Path C stability + flake hunt |
| [156](156-cleanup-build-warnings.md) | Build warning cleanup |
| [157](157-broader-flake-audit.md) | Broader smoke audit (Path A/B + non-wmterm Path C) |

## Reference

| Doc | Topic |
|---|---|
| [agent-api](agent-api.md) | AdventOS agent JSON-RPC API reference |
| [agent-cookbook](agent-cookbook.md) | Patterns for driving AdventOS via agentd |

---

## Reading paths

- **"How does fork/exec actually work?"** → `14`, then read `kernel/task.c`
  + `kernel/exec.c`.
- **"How does the WM work?"** → `94` → `98` → `99` → `100` → then any
  Path C session that touches the surface you care about.
- **"How is the agentd JSON-RPC wired?"** → `64`, then `agent-api.md`,
  then `agent-cookbook.md`.
- **"What's a 'Path'?"** → Roughly: Path A = bash-compat shell,
  Path B = compiler (`cc` + `tcc`), Path C = WM + apps,
  Path D = Lua interpreter, Path E = drivers.  The five paths were
  worked in parallel after session ~80.
- **"What's stable enough to depend on?"** → All major surfaces
  (kernel, fs, net, TLS, ssh, WM, shell, compiler).  See
  [`README.md`](../README.md) for the live "what works today"
  table, and `155` / `157` for stability methodology.
