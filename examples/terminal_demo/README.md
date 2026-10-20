# Terminal integration demo

A real world-panel CRT with a script-sized document, an animated aircraft marker,
and a walking camera. Uses public engine headers. The editor does not execute scripts.

Build from a Dev/source checkout:

```sh
cmake --build build/dev --target terminal_demo dengine_exe
build/dev/src/host/dengine --game "$PWD/build/dev/examples/terminal_demo/terminal_demo/libterminal_demo.dylib"
```

On macOS, a directly launchable app is also provided:

```sh
cmake --build build/dev --target terminal_demo_app
open build/dev/examples/terminal_demo/terminal_demo_app.app
```

The source manifest references the repository's licensed NotoSans test font through
an explicit read-only repository mount. This example needs source cooking; it is
not a packaged Shipping game. Other platforms use their normal `de_add_game` output.

- **WASD:** walk while outside the terminal. **E:** enter; **Escape:** return to walking.
  The application releases relative mouse capture for editing and reacquires it
  on exit, checking the platform result. SDL restricts capture to the focused window.
- **Click/drag:** place/select text. Keyboard selection, clipboard, undo/redo and
  shaped vertical navigation are the actual built-in editor. Documents grow past
  255 UTF-8 bytes; public get/set reads/writes the whole document.
- **F2** or **Ctrl+Space:** toggle the completion menu, constrained to the CRT's
  local extent. F2 avoids the macOS input-source shortcut. Click the suggestion
  to insert it; this is a fixed integration suggestion, not a language server.
- **F6:** pause/resume the aircraft marker only. UI frames and editing continue.
- **F8:** reset the example and start a new recording. **F9:** stop and save
  `user:/terminal-session.tuij` in the host application data directory. **F10:** reload that
  file, reset the session, and apply its commands in order. This verifies the final
  state immediately; it is not a real-time playback animation.

The bounded sample journal retains 4096 commands, with at most 1 MiB per text
payload and 16 MiB total payload; it reports a visible error rather than silently truncating a full log.
Stop recording promptly (ticks count too). The loader validates version, counts,
operation kinds, payload lengths, embedded NUL and exact EOF before installing a
recording. `.tuij` is this demo's companion format; `.deir` remains the engine's
resolved game-action format. UI selection/replacement/undo/redo are semantic
commands, so replay constructs actual undo history instead of installing text
snapshots. Transient IME composition and system clipboard contents are not saved;
the committed insertion is saved. Programmatic document set is baseline setup.

`ui_terminal.walk_edit_pause_save_and_fresh_world_replay` uses this exact controller
and journal in a fresh ECS/UI/input world, with no raw input injected during replay.
It verifies document content, focus/capture mode, pause state, walking and aircraft
counters and positions. Paused editing also runs UI frames without FixedUpdate.
`ui_terminal.crt_autocomplete_and_menu_stay_in_panel_on_resize` checks panel-edge
placement, screen DPI/safe-area independence, and oversized menus after CRT resize.
