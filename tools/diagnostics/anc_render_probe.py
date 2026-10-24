#!/usr/bin/env python3
"""Prepare an isolated ANC repro; never edits the source game or changes its pin.

Build: cmake --preset dev && cmake --build --preset dev --target anc -j 4
Run the issue's commands from the prepared directory. Every benchmark additionally writes
cold-warm.csv with all callback frames and delayed completed GPU pass timings. Performance
runs must not use ANC_PROBE_SEQUENCE. Set that variable to an existing directory and pass
--capture ignored.png for a deterministic 96-frame native temporal sequence (last 32 still).
Sequence readback synchronizes the GPU and is visual evidence, not performance evidence.
"""
import argparse
import hashlib
import json
import subprocess
import pathlib
import shutil

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=pathlib.Path, required=True)
p.add_argument('--engine', type=pathlib.Path, required=True)
p.add_argument('--out', type=pathlib.Path, required=True)
a = p.parse_args()
src, dst = a.source.resolve(), a.out.resolve()
if dst.exists():
    p.error('--out must not already exist')
dst.mkdir(parents=True)
for name in ('CMakeLists.txt', 'CMakePresets.json'):
    shutil.copy2(src/name, dst/name)
with (dst/'CMakeLists.txt').open('a') as f:
    f.write('\ntarget_include_directories(anc PRIVATE "${PROJECT_SOURCE_DIR}/engine/third_party/sdl3/SDL3-3.4.18/include")\n')
for name in ('game', 'tools'):
    shutil.copytree(src/name, dst/name, ignore=shutil.ignore_patterns('__pycache__'))
for name in ('art', 'ANC_Game_Agent_Handoff', 'aircraft-manufacturers', 'cargo-carrier-kits'):
    (dst/name).symlink_to(src/name, target_is_directory=True)
(dst/'engine').symlink_to(a.engine.resolve(), target_is_directory=True)
path = dst/'game/src/probe_benchmark.c'
s = path.read_text()
s = s.replace('static struct {', 'extern u64 anc_probe_start_ns;\nstatic struct {', 1)
needle = '    u64 now = de_time_now_ns();'
assert s.count(needle) == 1
s = s.replace(needle, needle + '''
    static FILE *trace;
    if (!trace) {
        char name[4096]; snprintf(name, sizeof name, "%s.cold-warm.csv", bench.path);
        trace = fopen(name, "w");
        if (trace) fputs("frame,interval_ms,cpu_record_ms,extract_ms,gpu_ms,pass,pass_gpu_ms\\n", trace);
    }
    if (trace) {
        const de_render_stats *stats = de_renderer_stats(e->renderer);
        fprintf(trace, "%u,%.6f,%.6f,%.6f,%.6f,frame,0\\n", bench.frames,
            (now-(bench.previous ? bench.previous : anc_probe_start_ns))/1e6, stats->cpu_record_ms,
            stats->extract_cpu_ms, stats->gpu_ms);
        for (u32 j=0; j<stats->pass_count; ++j)
            fprintf(trace, "%u,0,0,0,0,%s,%.6f\\n", bench.frames, stats->pass_names[j], stats->pass_gpu_ms[j]);
        fflush(trace);
    }''')
if 'frame_wait_ms;' in (a.engine/'include/dengine/de_render.h').read_text():
    s = s.replace('        fflush(trace);', '        fprintf(trace, "%u,0,0,0,0,CPU.frame_wait,%.6f\\n", bench.frames, stats->frame_wait_ms);\n        fflush(trace);')
if 'frame_acquire_ms' in (a.engine/'include/dengine/de_render.h').read_text():
    s = s.replace('        fflush(trace);', '        fprintf(trace, "%u,0,0,0,0,CPU.acquire,%.6f\\n", bench.frames, stats->frame_acquire_ms);\n        fprintf(trace, "%u,0,0,0,0,CPU.present,%.6f\\n", bench.frames, stats->frame_present_ms);\n        fflush(trace);')
path.write_text(s)
path = dst/'game/src/main.c'
s = path.read_text()
s = '#include <SDL3/SDL.h>\n' + s
s = s.replace('.debug_layer = true}', '.debug_layer = true, .pipeline_cache_dir = getenv("ANC_PROBE_COLD") ? "" : NULL}')
s = s.replace('capture_mouse(e, true)', 'capture_mouse(e, !motion_probe)')
s = s.replace('    ignore_mouse_delta = false;\n    if (motion_probe',
              '    ignore_mouse_delta = false;\n    if (getenv("ANC_PROBE_SEQUENCE") && motion_probe) { seated = false; terminal.fullscreen = false; }\n    if (motion_probe', 1)
s = s.replace('    if (motion_probe && !seated) {', '''    if (motion_probe && !seated)
        t->position = (de_dvec3){anc_playerspawn[0], anc_playerspawn[1], 1.68};
    if (motion_probe && !seated) {''')
s = s.replace('int main(int argc, char **argv) {', 'u64 anc_probe_start_ns;\nint main(int argc, char **argv) {\n    anc_probe_start_ns = de_time_now_ns();')
s = s.replace('static bool capture(de_engine *e) {', 'static bool capture(de_engine *e) {\n    stbi_write_png_compression_level = 1;')
s = s.replace('const u32 width = 1280, height = 800, pitch = width * 4;',
              'const u32 width = 2560, height = 1600, pitch = width * 4;')
# Only the capture target, not the window or 3D budget.
a0 = s.index('capture_target = de_gpu_texture_create(')
b0 = s.index('capture_view = de_view_create(', a0)
s = s[:a0] + s[a0:b0].replace('1280', '2560').replace('800', '1600') + s[b0:]
s = s.replace('yaw = .95f * sinf(++motion_frame * .02f);',
'''if (!getenv("ANC_PROBE_SEQUENCE") || motion_frame < 64) ++motion_frame;
        yaw = .95f * sinf(motion_frame * .02f);''')
if '        ++motion_frame;' in s:
    s = s.replace('        ++motion_frame;', '        if (!getenv("ANC_PROBE_SEQUENCE") || motion_frame < 64) ++motion_frame;', 1)
needle = '    if (capture_path && ++frame_number == 25) {'
assert s.count(needle) == 1
s = s.replace(needle, '''    if (getenv("ANC_PROBE_TRANSITIONS")) {
        static unsigned step; ++step;
        if (step % 120 == 30) { set_seated(e, true); terminal.fullscreen = false; }
        if (step % 120 == 60) terminal.fullscreen = true;
        if (step % 120 == 90) set_seated(e, false);
        if (step % 120 == 0) terminal.fullscreen = false;
        SDL_Window *window = (SDL_Window *)de_window_native(e->window).sdl_window;
        if (step == 120) SDL_SetWindowSize(window, 640, 400);
        if (step == 240) SDL_SetWindowSize(window, 1280, 800);
        if (step == 150) SDL_HideWindow(window);
        if (step == 160) { SDL_ShowWindow(window); SDL_RaiseWindow(window); }
        printf("ANC_PROBE_MODE %u %u %u\\n", step, seated, terminal.fullscreen);
    }
    if (capture_path && getenv("ANC_PROBE_SEQUENCE")) {
        static char sequence_path[4096];
        ++frame_number;
        snprintf(sequence_path, sizeof sequence_path, "%s/%04u.png", getenv("ANC_PROBE_SEQUENCE"), frame_number);
        capture_path = sequence_path;
        if (!capture(e)) probe_result = 2;
        printf("ANC_PROBE_POSE %u %u %.9g %.9g\\n", frame_number, motion_frame, yaw, pitch);
        static FILE *poses;
        if (!poses) {
            char path[4096]; snprintf(path, sizeof path, "%s/poses.csv", getenv("ANC_PROBE_SEQUENCE"));
            poses = fopen(path, "w");
            if (poses) fputs("frame,motion_frame,x,y,z,yaw,pitch,projection,fov_y\\n", poses);
        }
        if (poses) {
            fprintf(poses, "%u,%u,%.17g,%.17g,%.17g,%.9g,%.9g,%u,%.9g\\n", frame_number,
                motion_frame, t->position.x, t->position.y, t->position.z, yaw, pitch,
                camera->projection, camera->fov_y);
            fflush(poses);
        }
        if (frame_number == 96) de_engine_request_quit(e);
    } else if (capture_path && ++frame_number == 25) {''')
path.write_text(s)
print(dst)

# Record the copied inputs, rather than the potentially changing live checkout.
def revision(root):
    result = subprocess.run(['git', '-C', str(root), 'rev-parse', 'HEAD'],
                            capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None
manifest = {
    'source': str(src), 'source_revision': revision(src),
    'engine': str(a.engine.resolve()), 'engine_revision': revision(a.engine),
    'probe_sources_sha256': {
        str(p.relative_to(dst)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in (dst/'game/src').rglob('*') if p.is_file()
    },
    'camera': 'standing eye height 1.68m; fixed spawn XY; 64 moving + 32 still frames',
    'limits': 'Readback captures are not performance measurements. Pipeline-cold is not clean-disk cold.'
}
(dst/'anc-probe-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
