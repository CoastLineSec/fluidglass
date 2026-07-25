# Lua configuration reference

Availability: planned for HyprFluidGlass v2; not available in the current
release.

HyprFluidGlass v2 uses Hyprland's native Lua configuration API for durable
materials and attachment rules. Runtime shells should use the session API
instead of rewriting durable configuration.

## Entry point

```lua
hl.plugin.hyprfluidglass.configure(snapshot)
```

`snapshot` is one complete versioned configuration. Calling the function more
than once during one Hyprland reload replaces the pending snapshot rather than
merging partial tables.

## Minimal configuration

```lua
hl.plugin.hyprfluidglass.configure({
    version = 2,
    enabled = true,
    default_material = "fluid",
    materials = {
        fluid = {},
    },
    window_rules = {},
    layer_rules = {},
})
```

## Complete example

```lua
hl.plugin.hyprfluidglass.configure({
    version = 2,
    enabled = true,
    default_material = "fluid",

    materials = {
        fluid = {
            glass_level = 0.50,
            blur_level = 0.50,
            tint_level = 0.20,
            tint_enabled = true,
            tint_color = "#DCEBFF",
            light_mode = false,

            refraction = 45.0,
            rim_band = 30.0,
            bevel = 30.0,
            rim_width = 3.0,
            highlight = 0.10,
            shadow = 0.10,
            light_angle = 90.0,
            specular = 0.21,
            chroma = 0.15,
            edge_depth = 0.14,
            lens = 0.12,
            lens_band = 40.0,
            gloss = 0.14,
        },

        clear = {
            glass_level = 0.20,
            blur_level = 0.15,
            tint_enabled = false,
            refraction = 34.0,
            gloss = 0.10,
        },
    },

    window_rules = {
        {
            id = "file-manager",
            match = {
                initial_class = { exact = "org.gnome.Nautilus" },
            },
            material = "fluid",
            enabled = true,
        },
    },

    layer_rules = {
        {
            id = "desktop-shell",
            match = {
                namespace = {
                    regex = "^example-shell:",
                },
            },
            material = "fluid",
            enabled = true,
        },
    },
})
```

## Snapshot fields

| Field | Type | Required | Meaning |
|---|---|---:|---|
| `version` | integer | yes | Must be exactly `2` |
| `enabled` | boolean | yes | Global durable-rule enable state |
| `default_material` | string | yes | Name in `materials` |
| `materials` | table | yes | Named material definitions |
| `window_rules` | array | yes | Durable application-glass rules |
| `layer_rules` | array | yes | Durable layer-surface rules |

Unknown top-level fields are rejected. A missing required field invalidates the
pending snapshot.

## Reload transaction

Configuration reloads are fail-safe:

1. Hyprland's `preReload` event creates an empty staging transaction.
2. `configure()` validates and stores one complete pending snapshot.
3. Hyprland's `reloaded` event commits the pending snapshot.
4. If no valid v2 snapshot was received, the last known-good configuration
   remains active.

An invalid reload is reported in plugin status and logs. It never clears the
currently working configuration.

## Material definitions

All material fields are optional. Omitted values use the compatibility defaults
shown below.

| Field | Type | Default | Accepted range |
|---|---|---:|---:|
| `glass_level` | number | `0.50` | `0.0`–`1.0` |
| `blur_level` | number | derived | `0.0`–`1.0` |
| `tint_level` | number | derived | `0.0`–`1.0` |
| `tint_enabled` | boolean | `false` | — |
| `tint_color` | string | `"#FFFFFF"` | `#RRGGBB` |
| `light_mode` | boolean | `false` | — |
| `refraction` | number | `45.0` | `0.0`–`200.0` |
| `rim_band` | number | `30.0` | `0.0`–`200.0` |
| `bevel` | number | `30.0` | `0.0`–`200.0` |
| `rim_width` | number | `3.0` | `0.0`–`50.0` |
| `highlight` | number | `0.10` | `0.0`–`2.0` |
| `shadow` | number | `0.10` | `0.0`–`2.0` |
| `light_angle` | number | `90.0` | `0.0`–`360.0` |
| `specular` | number | `0.21` | `0.0`–`2.0` |
| `chroma` | number | `0.15` | `0.0`–`1.0` |
| `edge_depth` | number | `0.14` | `0.0`–`2.0` |
| `lens` | number | `0.12` | `0.0`–`1.0` |
| `lens_band` | number | `40.0` | `0.0`–`200.0` |
| `gloss` | number | `0.14` | `0.0`–`2.0` |

`blur_level` and `tint_level` derive from `glass_level` when omitted. Every
number must be finite. `NaN`, positive or negative infinity, numeric strings,
and values outside the accepted range are rejected rather than silently
coerced.

Material names:

- must be non-empty UTF-8 strings;
- are limited to 128 bytes;
- must be unique inside the snapshot;
- may contain ASCII letters, digits, `_`, `-`, and `.`;
- cannot start with the reserved prefix `_hfg_`.

## Match expressions

A match field contains exactly one mode:

```lua
{ exact = "org.gnome.Nautilus" }
```

or:

```lua
{ regex = "^org\\.gnome\\." }
```

Rules:

- empty exact values and empty regular expressions are invalid;
- regular-expression source is limited to 256 bytes;
- regular expressions are compiled once when the snapshot is validated;
- an invalid expression rejects the complete pending snapshot;
- matching does not compile expressions or allocate an unbounded cache in a
  render callback.

Exact matching is the default recommendation. Use a regular expression only
when one rule genuinely needs to match a controlled family of surfaces.

## Window rules

```lua
{
    id = "file-manager",
    match = {
        initial_class = { exact = "org.gnome.Nautilus" },
    },
    material = "fluid",
    enabled = true,
}
```

| Field | Type | Required | Meaning |
|---|---|---:|---|
| `id` | string | yes | Stable rule identity |
| `match.initial_class` | match expression | at least one | Initial app class |
| `match.class` | match expression | at least one | Current app class |
| `match.initial_title` | match expression | at least one | Initial window title |
| `match.title` | match expression | at least one | Current window title |
| `material` | string | yes | Material name |
| `enabled` | boolean | no | Defaults to `true` |

A window rule must contain at least one match field. Multiple fields in one
rule are combined with logical AND.

Window rules create generic under-window glass attachments. They do not alter
GTK or application themes. An application must expose transparent regions for
the glass to be visible.

## Layer rules

```lua
{
    id = "desktop-shell",
    match = {
        namespace = { regex = "^example-shell:" },
    },
    material = "fluid",
    enabled = true,
}
```

| Field | Type | Required | Meaning |
|---|---|---:|---|
| `id` | string | yes | Stable rule identity |
| `match.namespace` | match expression | yes | Layer-shell namespace |
| `material` | string | yes | Material name |
| `enabled` | boolean | no | Defaults to `true` |

The plugin treats the namespace as an opaque client-controlled identifier. It
does not grant special behavior to `hgs:`, `quickshell:`, `ags:`, or any other
prefix.

## Rule precedence

Durable rules are evaluated in array order:

1. disabled rules are skipped;
2. the first matching rule supplies the material;
3. a runtime target owned by a live session takes precedence for that exact
   attachment;
4. a preview target takes precedence only for its explicitly selected target
   and expires with its lease.

Configuration authorities remain separate. A runtime replacement cannot delete
or mutate a durable rule.

## Validation result

`configure()` returns a structured Lua result:

```lua
{
    ok = true,
    version = 2,
    material_count = 2,
    window_rule_count = 1,
    layer_rule_count = 1,
}
```

On failure:

```lua
{
    ok = false,
    error = {
        code = "invalid-material",
        path = "materials.fluid.tint_color",
        message = "expected #RRGGBB",
    },
}
```

The error path identifies the rejected field without echoing unrelated
configuration or sensitive window metadata.
