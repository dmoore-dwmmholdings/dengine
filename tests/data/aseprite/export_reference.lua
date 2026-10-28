-- Export through Aseprite's public compositor, including group and layer opacity.
-- A fresh RGBA canvas also exercises indexed/grayscale conversion through the renderer.
local source = assert(app.params["source"])
local prefix = assert(app.params["output"])
local selected = app.params["layer"]
app.preferences.experimental.compose_groups = app.params["groups"] == "true"
local sprite = assert(app.open(source))
if selected and selected ~= "" then
  local found = 0
  local function select_layers(layers)
    for _, layer in ipairs(layers) do
      if layer.isGroup then
        select_layers(layer.layers)
      else
        local keep = layer.isVisible and layer.name == selected
        layer.isVisible = keep
        if keep then found = found + 1 end
      end
    end
  end
  select_layers(sprite.layers)
  assert(found == 1, "expected one visible image layer named " .. selected)
end
for i, frame in ipairs(sprite.frames) do
  local image = Image(sprite.width, sprite.height, ColorMode.RGB)
  image:clear()
  image:drawSprite(sprite, frame)
  assert(image:saveAs(prefix .. "-" .. (i - 1) .. ".png"))
end
sprite:close()
