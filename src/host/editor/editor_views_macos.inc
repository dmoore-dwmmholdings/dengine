/* macOS shared-frame transport. Included after the private view types. The engine renders
 * with Metal; the IDE consumes a protected pixel snapshot. No global IOSurface IDs or ports. */
static bool bridge_init(const char *adapter) {
    (void)adapter;
    memcpy(g_views.bridge.adapter, "metal", 6);
    g_views.bridge_ok = true;
    return true;
}

static void ring_free(ed_ring *r) {
    for (u32 i = 0; i < ED_RING; ++i) {
        if (r->map[i] != NULL) munmap(r->map[i], r->bytes);
        if (r->path[i][0] != '\0') unlink(r->path[i]);
    }
    memset(r, 0, sizeof *r);
}

static bool ring_create(ed_view *v, u32 w, u32 h) {
    if (!w || !h || w > 8192 || h > 8192) return false;
    if (v->ring_count == ED_MAX_RINGS) {
        ring_free(&v->rings[0]);
        memmove(v->rings, v->rings + 1, sizeof(ed_ring) * (ED_MAX_RINGS - 1));
        --v->ring_count;
    }
    ed_ring *r = &v->rings[v->ring_count];
    memset(r, 0, sizeof *r);
    r->generation = v->next_generation++;
    r->w = w; r->h = h;
    r->bytes = 64u + (size_t)w * h * 4u;
    for (u32 i = 0; i < ED_RING; ++i) {
        snprintf(r->path[i], sizeof r->path[i], "/tmp/dengine-view-XXXXXX");
        const int fd = mkstemp(r->path[i]);
        if (fd < 0) { r->path[i][0] = '\0'; ring_free(r); return false; }
        const bool sized = fchmod(fd, 0600) == 0 && ftruncate(fd, (off_t)r->bytes) == 0;
        void *map = sized ? mmap(NULL, r->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        close(fd);
        if (map == MAP_FAILED) { ring_free(r); return false; }
        r->map[i] = map;
        memset(map, 0, 64);
        u32 *header = (u32 *)map;
        header[2] = w; header[3] = h; header[4] = w * 4u; header[5] = 0x44455631u;
    }
    ++v->ring_count;
    return true;
}

static bool view_gpu_create(ed_view *v) {
    v->next_generation = 1;
    return ring_create(v, v->sw, v->sh);
}

static void view_gpu_free(ed_view *v) {
    for (u32 i = 0; i < v->ring_count; ++i) ring_free(&v->rings[i]);
    v->ring_count = 0;
}
