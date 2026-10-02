# Card configuration selectors

The plugin owns its configuration schema and validates it during creation.
The host uses `SrhCardDescriptor` metadata to bind generic controls to that schema.
It does not infer resource roles from plugin IDs or JSON key names.

- `SRH_CARD_REQUIRES_IO_SPACE` and `io_space_config_key` declare an I/O space selector.
- `memory_space_config_key` declares a separate memory resource selector.
  `memory_space_label` supplies its label, or the host uses "Memory space".
- `base_config_key` binds the card base control to a config key.

The memory fields are optional descriptor tail fields. Consumers check
`struct_size` before reading them. Older plugins continue without a memory selector.
The engine copies the metadata into `SrzPluginDescriptor` before unloading the
plugin library. The UI receives owned copies on the simulation worker boundary.

When adding a card, a declared resource's default name is selected if it exists
in the project. Otherwise the selected mapping space is used. Card info loads
the saved resource names, retaining missing resources as unselected so applying
an unrelated edit cannot silently change a resource binding.

Controls own their declared config keys. Those keys are excluded from the JSON
editor and written from the selected controls during insertion or configuration.
Other plugin settings remain editable as JSON. Project serialization and card
creation receive the complete configuration object.
