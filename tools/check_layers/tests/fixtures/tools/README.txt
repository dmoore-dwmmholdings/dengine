src/render includes a tools/pack header; src/gpu includes the cooker library (gpu has no allow
edge to cook; render has had one since DENG-176, for its built-in shaders); src/asset includes the
cooker library (allowed). Expected: exit 1, asset not reported.
