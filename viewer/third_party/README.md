# JSON parser

`json.hpp` is nlohmann/json 3.11.3, vendored as a standalone header for deterministic offline builds. Its MIT license and copyright notice are embedded in the file. The source copy came from the installed GEOS 3.14.0 dependency's `include/geos/vend/json.hpp`, where the upstream header was already available locally. It is used only for JSON parsing/serialization; no GEOS linking is required.

Upstream project: https://github.com/nlohmann/json/tree/v3.11.3
