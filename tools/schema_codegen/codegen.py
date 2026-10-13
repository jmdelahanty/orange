#!/usr/bin/env python3
"""Generate C++ structs, writers and strict readers from Orange's published JSON Schemas.

  codegen.py <schema.json> <output header> --namespace NAME [--ref SCHEMA_STEM=NAMESPACE]...

Design: docs/schema_first_outputs_design_2026-10-12.md. The generator accepts a deliberate
subset of JSON Schema and refuses anything else, so a schema never says more than the generated
code enforces:

  type (object, array, string, integer, number, boolean; ["T", "null"] for nullable), properties,
  required, additionalProperties (false; a schema for string-keyed maps, whose keys a
  propertyNames {pattern} may constrain; or absent/true on an
  object with properties, which is "open": declared fields are typed and checked, any other keys
  are kept verbatim in `extra` and written back unchanged), items, enum, const,
  minimum / maximum, minLength / maxLength, pattern, oneOf with one const discriminator property,
  $defs and local $ref (each $defs entry is one type, named <Root><Key>), plus the x-orange-* annotations (x-orange-cpp-name, x-orange-decimals).
  On an object: allOf of {if, then} rules, where `if` tests const / enum values below nested
  properties and `then` constrains values below nested properties (const, enum, type, minLength,
  maxLength, pattern, minimum, maximum). They are checked on the parsed JSON with JSON Schema's
  semantics: an absent value satisfies a `properties` condition and is not constrained. `then`
  may also list `required` keys; an `else` takes the same rules, and both accept
  `not: {required: [...]}` (keys that must be absent).
  A `$ref` to another schema file in the same directory ("<stem>.schema.json") is that file's
  generated root type, from its header (the stem must be passed with --ref and its namespace).
  A `oneOf` below the root, whose branches are objects or such references, is a std::variant;
  parse requires exactly one branch to match, as JSON Schema does.

Generated per object schema:
  struct T { typed fields in schema order };      const fields are not stored
  nlohmann::json to_json(const T&);               consts filled in, numbers rounded as declared
  bool parse(const nlohmann::json&, T*, std::string* error);   exact keys, types, consts, enums,
                                                                ranges, lengths, patterns
An optional const is not stored but its presence is (`bool has_<name>`), so it round-trips.
For a root oneOf: a std::variant of the branch structs, dispatched on the discriminator.

Only the standard library is used, so the generator runs at CMake configure time anywhere.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

SUPPORTED = {
    "type", "properties", "required", "additionalProperties", "items", "enum", "const",
    "minimum", "maximum", "minLength", "maxLength", "pattern", "oneOf", "$defs", "$ref", "allOf",
    "description", "title", "$schema", "$id", "examples", "default",
    "x-orange-cpp-name", "x-orange-decimals", "x-orange-counter", "x-orange-digest",
    "x-orange-map-key", "propertyNames",
}


class SchemaError(Exception):
    pass


def camel(text: str) -> str:
    parts = re.split(r"[^A-Za-z0-9]+", text)
    return "".join(p[:1].upper() + p[1:] for p in parts if p)


def cpp_identifier(name: str) -> str:
    ident = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if ident[:1].isdigit():
        ident = "_" + ident
    if ident in {"default", "class", "struct", "namespace", "operator", "template", "union",
                 "enum", "register", "delete", "new", "this", "auto", "const", "int", "bool",
                 "double", "float", "char", "long", "short", "signed", "unsigned", "void"}:
        ident += "_"
    return ident


# ---- type model -------------------------------------------------------------------------------

@dataclass
class Scalar:
    kind: str                     # string | int64 | uint64 | double | bool
    enum: list | None = None
    minimum: float | None = None
    maximum: float | None = None
    min_length: int | None = None
    max_length: int | None = None
    pattern: str | None = None
    decimals: int | None = None


@dataclass
class ArrayT:
    item: object


@dataclass
class MapT:
    value: object
    key_pattern: str | None = None   # propertyNames {pattern}


@dataclass
class OpaqueT:                    # a value whose shape the schema leaves open: kept as JSON
    types: tuple = ("object",)    # the JSON types the schema allows; () allows any


@dataclass
class Field:
    json_name: str
    cpp_name: str
    type: object                  # None when const
    required: bool
    nullable: bool = False
    const: object = None
    is_const: bool = False


@dataclass
class Rule:                       # one leaf of an if / then tree
    path: tuple                   # property names from the object down to the value
    checks: dict                  # keyword -> value (const, enum, type, minLength, ...)


@dataclass
class Condition:                  # one allOf entry: if every `when` rule holds, every `then` must;
    index: int                    # otherwise every `otherwise` (`else`) rule must
    when: list
    then: list
    otherwise: list = field(default_factory=list)


@dataclass
class ObjectT:
    name: str
    fields: list = field(default_factory=list)
    open: bool = False            # additionalProperties absent/true: undeclared keys kept in extra
    conditions: list = field(default_factory=list)


@dataclass
class ExternalT:                  # the root type of another published schema file
    stem: str                     # file name without .schema.json
    namespace: str
    type_name: str


@dataclass
class OneOfT:                     # a oneOf below the root: exactly one branch must match
    branches: list


@dataclass
class VariantT:
    name: str
    discriminator: str
    branches: list = field(default_factory=list)   # [(discriminator value, ObjectT)]


class Builder:
    def __init__(self, root: dict, schema_dir: Path | None = None, refs: dict | None = None):
        self.root = root
        self.schema_dir = schema_dir
        self.refs = refs or {}                # schema stem -> generated namespace
        self.externals: dict[str, ExternalT] = {}
        self.objects: list[ObjectT] = []     # emission order: dependencies first
        self.root_name = ""
        self.def_types: dict[str, object] = {}   # $defs key -> its type, generated once

    def external(self, node: dict, where: str):
        """ExternalT for a {"$ref": "<stem>.schema.json"} node, else None."""
        ref = node.get("$ref") if isinstance(node, dict) else None
        if not isinstance(ref, str) or ref.startswith("#"):
            return None
        if not ref.endswith(".schema.json") or "/" in ref or set(node) - {"$ref", "description"}:
            raise SchemaError(f"{where}: only a whole-file $ref to a sibling schema is supported, got {ref}")
        stem = ref[: -len(".schema.json")]
        if stem not in self.refs:
            raise SchemaError(f"{where}: {ref} is not a generated schema (pass --ref {stem}=<namespace>)")
        if stem not in self.externals:
            target = json.loads((self.schema_dir / ref).read_text(encoding="utf-8"))
            type_name = target.get("x-orange-cpp-name") or camel(stem)
            self.externals[stem] = ExternalT(stem, self.refs[stem], type_name)
        return self.externals[stem]

    def resolve(self, node: dict) -> dict:
        while "$ref" in node:
            ref = node["$ref"]
            if not ref.startswith("#/$defs/"):
                raise SchemaError(f"only local #/$defs references are supported, got {ref}")
            node = self.root["$defs"][ref[len("#/$defs/"):]]
        return node

    def check_keywords(self, node: dict, where: str) -> None:
        unknown = set(node) - SUPPORTED
        if unknown:
            raise SchemaError(f"{where}: unsupported keywords {sorted(unknown)}")

    def type_of(self, node: dict, name: str, where: str):
        external = self.external(node, where)
        if external is not None:
            return external
        ref = node.get("$ref") if isinstance(node, dict) else None
        if isinstance(ref, str) and ref.startswith("#/$defs/"):
            # A $defs entry is one type named after its key, however many places use it.
            key = ref[len("#/$defs/"):]
            if key not in self.def_types:
                definition = self.root["$defs"][key]
                self.def_types[key] = self.type_of(
                    definition, definition.get("x-orange-cpp-name", self.root_name + camel(key)),
                    f"#/$defs/{key}")
            return self.def_types[key]
        node = self.resolve(node)
        self.check_keywords(node, where)
        types = node.get("type")
        if isinstance(types, list):
            non_null = [t for t in types if t != "null"]
            if len(non_null) != 1:
                # A union of several types: the value stays JSON, checked against those types.
                if any(k in node for k in ("properties", "items", "enum", "const")):
                    raise SchemaError(f"{where}: a {types} union with a nested shape is not supported")
                return OpaqueT(tuple(types))
            if len(types) > 2:
                raise SchemaError(f"{where}: duplicate types {types}")
            types = non_null[0]
        if "oneOf" in node:
            if set(node) - {"oneOf", "description", "title"}:
                raise SchemaError(f"{where}: a oneOf below the root must stand alone")
            branches = [self.type_of(b, f"{name}Option{i}", f"{where}/oneOf[{i}]")
                        for i, b in enumerate(node["oneOf"])]
            if not all(isinstance(b, (ObjectT, ExternalT)) for b in branches):
                raise SchemaError(f"{where}: oneOf branches must be objects or schema references")
            return OneOfT(branches)
        if "allOf" in node and "properties" not in node:
            raise SchemaError(f"{where}: allOf is supported only on an object with properties")
        if types == "object" or (types is None and "properties" in node):
            props = node.get("properties")
            additional = node.get("additionalProperties", True)
            if props is None:
                if isinstance(additional, dict):
                    names = node.get("propertyNames")
                    if names is not None and (not isinstance(names, dict) or set(names) != {"pattern"}):
                        raise SchemaError(f"{where}: propertyNames supports only {{pattern}}")
                    return MapT(self.type_of(additional, name + "Value", where + "/*"),
                                key_pattern=names["pattern"] if names else None)
                return OpaqueT()
            if additional not in (False, True):
                raise SchemaError(f"{where}: additionalProperties must be false or true on an object with properties")
            return self.object(node, node.get("x-orange-cpp-name", name), where)
        if types == "array":
            if "items" not in node:
                return OpaqueT(("array",))   # items left open by the schema
            return ArrayT(self.type_of(node["items"], name + "Item", where + "[]"))
        if types in ("string", "integer", "number", "boolean"):
            kind = {"string": "string", "number": "double", "boolean": "bool"}.get(types)
            if types == "integer":
                minimum = node.get("minimum")
                kind = "uint64" if minimum is not None and minimum >= 0 else "int64"
            return Scalar(kind=kind, enum=node.get("enum"), minimum=node.get("minimum"),
                          maximum=node.get("maximum"), min_length=node.get("minLength"),
                          max_length=node.get("maxLength"), pattern=node.get("pattern"),
                          decimals=node.get("x-orange-decimals"))
        if types is None and "enum" in node and all(isinstance(v, str) for v in node["enum"]):
            return Scalar(kind="string", enum=node["enum"])
        if types is None and not any(k in node for k in ("properties", "items", "enum", "const")):
            return OpaqueT(())             # any JSON value
        raise SchemaError(f"{where}: cannot map type {types!r}")

    IF_CHECKS = {"const", "enum"}
    THEN_CHECKS = {"const", "enum", "type", "minLength", "maxLength", "pattern", "minimum", "maximum",
                   "required"}
    TREE_NOTES = {"description", "title"}

    def rules(self, node: dict, allowed: set, where: str, path: tuple = ()) -> list:
        """Flatten an if / then tree of nested `properties` into leaf rules."""
        if not isinstance(node, dict):
            raise SchemaError(f"{where}: expected a schema object")
        unknown = set(node) - allowed - {"properties"} - self.TREE_NOTES
        if unknown:
            raise SchemaError(f"{where}: unsupported keywords in a condition {sorted(unknown)}")
        out = []
        checks = {k: v for k, v in node.items() if k in allowed and k != "not"}
        if "not" in node and "not" in allowed:
            negated = node["not"]
            if not isinstance(negated, dict) or set(negated) != {"required"}:
                raise SchemaError(f"{where}: only not {{required: [...]}} is supported in a condition")
            checks["forbidden"] = negated["required"]
        if checks:
            if "type" in checks and not isinstance(checks["type"], str):
                raise SchemaError(f"{where}: a condition type must be one type name")
            out.append(Rule(path, checks))
        for key, sub in (node.get("properties") or {}).items():
            out += self.rules(sub, allowed, f"{where}/{key}", path + (key,))
        return out

    def conditions(self, node: dict, where: str) -> list:
        out = []
        for index, entry in enumerate(node.get("allOf", [])):
            if not isinstance(entry, dict) or set(entry) - {"if", "then", "else"} - self.TREE_NOTES or \
                    "if" not in entry or "then" not in entry:
                raise SchemaError(f"{where}/allOf[{index}]: only {{if, then[, else]}} entries are supported")
            when = self.rules(entry["if"], self.IF_CHECKS, f"{where}/allOf[{index}]/if")
            then = self.rules(entry["then"], self.THEN_CHECKS | {"not"}, f"{where}/allOf[{index}]/then")
            otherwise = (self.rules(entry["else"], self.THEN_CHECKS | {"not"}, f"{where}/allOf[{index}]/else")
                         if "else" in entry else [])
            if not when or not then:
                raise SchemaError(f"{where}/allOf[{index}]: an empty if or then")
            out.append(Condition(index, when, then, otherwise))
        return out

    def object(self, node: dict, name: str, where: str) -> ObjectT:
        obj = ObjectT(name=name, open=node.get("additionalProperties", True) is True)
        obj.conditions = self.conditions(node, where)
        required = set(node.get("required", []))
        for json_name, sub in node["properties"].items():
            sub_r = sub if self.external(sub, f"{where}/{json_name}") else self.resolve(sub)
            self.check_keywords(sub_r, f"{where}/{json_name}")
            nullable = (isinstance(sub_r.get("type"), list) and "null" in sub_r["type"] and
                        len(sub_r["type"]) == 2)
            if "const" in sub_r:
                obj.fields.append(Field(json_name, cpp_identifier(json_name), None,
                                        json_name in required, const=sub_r["const"], is_const=True))
                continue
            t = self.type_of(sub, name + camel(json_name), f"{where}/{json_name}")
            obj.fields.append(Field(json_name, cpp_identifier(json_name), t,
                                    json_name in required, nullable=nullable))
        self.objects.append(obj)
        return obj

    def build(self, root_name: str):
        self.root_name = root_name
        root = self.root
        self.check_keywords(root, "#")
        if "oneOf" in root:
            if "allOf" in root:
                raise SchemaError("#: allOf beside a root oneOf is not supported")
            branches = [self.resolve(b) for b in root["oneOf"]]
            discriminator = None
            for candidate in branches[0].get("properties", {}):
                values = [b.get("properties", {}).get(candidate, {}).get("const") for b in branches]
                if all(v is not None for v in values) and len(set(map(json.dumps, values))) == len(values):
                    discriminator = candidate
                    break
            if discriminator is None:
                raise SchemaError("#: oneOf branches need one const property with distinct values")
            variant = VariantT(root_name, discriminator)
            for b in branches:
                value = b["properties"][discriminator]["const"]
                variant.branches.append((value, self.object(b, root_name + camel(str(value)), f"#/oneOf/{value}")))
            return variant
        return self.object(root, root.get("x-orange-cpp-name", root_name), "#")


# ---- C++ emission -------------------------------------------------------------------------------

def cpp_type(t) -> str:
    if isinstance(t, Scalar):
        return {"string": "std::string", "int64": "std::int64_t", "uint64": "std::uint64_t",
                "double": "double", "bool": "bool"}[t.kind]
    if isinstance(t, ArrayT):
        return f"std::vector<{cpp_type(t.item)}>"
    if isinstance(t, MapT):
        return f"std::map<std::string, {cpp_type(t.value)}>"
    if isinstance(t, OpaqueT):
        return "nlohmann::json"
    if isinstance(t, ExternalT):
        return f"::orange::outputs::gen::{t.namespace}::{t.type_name}"
    if isinstance(t, OneOfT):
        return "std::variant<" + ", ".join(cpp_type(b) for b in t.branches) + ">"
    if isinstance(t, ObjectT):
        return t.name
    raise TypeError(t)


def json_literal(value) -> str:
    """C++ expression producing the nlohmann::json for a const value."""
    if isinstance(value, bool):
        return "nlohmann::json(true)" if value else "nlohmann::json(false)"
    if isinstance(value, int):
        return f"nlohmann::json({value})" if value < 0 else f"nlohmann::json(std::uint64_t{{{value}}})"
    if isinstance(value, float):
        return f"nlohmann::json({value!r})"
    if isinstance(value, str):
        return f"nlohmann::json({json.dumps(value)})"
    if value is None:
        return "nlohmann::json(nullptr)"
    return f"nlohmann::json::parse({json.dumps(json.dumps(value))})"


class Emitter:
    def __init__(self):
        self.lines: list[str] = []
        self.regex_count = 0

    def w(self, line: str = "") -> None:
        self.lines.append(line)

    def write_expr(self, t, expr: str) -> str:
        if isinstance(t, Scalar):
            if t.kind == "double" and t.decimals is not None:
                return f"nlohmann::json(detail::round_to({expr}, {t.decimals}))"
            return f"nlohmann::json({expr})"
        if isinstance(t, ObjectT):
            return f"to_json({expr})"
        if isinstance(t, OpaqueT):
            return expr
        if isinstance(t, ExternalT):
            return f"::orange::outputs::gen::{t.namespace}::to_json({expr})"
        if isinstance(t, OneOfT):
            # Each alternative's to_json lives in its own namespace; ADL finds it.
            return f"std::visit([](const auto& alt) {{ return nlohmann::json(to_json(alt)); }}, {expr})"
        if isinstance(t, ArrayT):
            return (f"[&] {{ nlohmann::json a = nlohmann::json::array(); for (const auto& v : {expr}) "
                    f"a.push_back({self.write_expr(t.item, 'v')}); return a; }}()")
        if isinstance(t, MapT):
            return (f"[&] {{ nlohmann::json m = nlohmann::json::object(); for (const auto& [k, v] : {expr}) "
                    f"m[k] = {self.write_expr(t.value, 'v')}; return m; }}()")
        raise TypeError(t)

    def parse_stmt(self, t, src: str, dst: str, path: str, indent: str) -> list[str]:
        """Statements parsing json `src` into `dst`; on failure set error and return false."""
        out: list[str] = []
        fail = lambda msg: f'{{ return detail::fail(error, {path} + ": {msg}"); }}'
        if isinstance(t, Scalar):
            if t.kind == "string":
                out.append(f"if (!{src}.is_string()) {fail('expected a string')}")
                out.append(f"{dst} = {src}.get<std::string>();")
                if t.enum is not None:
                    allowed = ", ".join(json.dumps(v) for v in t.enum)
                    out.append(f"if (!detail::one_of({dst}, {{{allowed}}})) {fail('value not in enum')}")
                if t.min_length is not None:
                    out.append(f"if ({dst}.size() < {t.min_length}u) {fail('string too short')}")
                if t.max_length is not None:
                    out.append(f"if ({dst}.size() > {t.max_length}u) {fail('string too long')}")
                if t.pattern is not None:
                    self.regex_count += 1
                    out.append(f"static const std::regex pattern_{self.regex_count}({json.dumps(t.pattern)}, std::regex::ECMAScript);")
                    out.append(f"if (!std::regex_search({dst}, pattern_{self.regex_count})) {fail('does not match pattern')}")
            elif t.kind == "bool":
                out.append(f"if (!{src}.is_boolean()) {fail('expected a boolean')}")
                out.append(f"{dst} = {src}.get<bool>();")
            elif t.kind == "uint64":
                out.append(f"if (!{src}.is_number_unsigned() && !({src}.is_number_integer() && {src}.get<std::int64_t>() >= 0)) {fail('expected a non-negative integer')}")
                out.append(f"{dst} = {src}.get<std::uint64_t>();")
            elif t.kind == "int64":
                out.append(f"if (!{src}.is_number_integer()) {fail('expected an integer')}")
                out.append(f"{dst} = {src}.get<std::int64_t>();")
            elif t.kind == "double":
                out.append(f"if (!{src}.is_number()) {fail('expected a number')}")
                out.append(f"{dst} = {src}.get<double>();")
            if t.kind in ("uint64", "int64", "double"):
                if t.minimum is not None:
                    out.append(f"if (static_cast<double>({dst}) < {float(t.minimum)!r}) {fail('below minimum')}")
                if t.maximum is not None:
                    out.append(f"if (static_cast<double>({dst}) > {float(t.maximum)!r}) {fail('above maximum')}")
        elif isinstance(t, ObjectT):
            out.append(f"if (!parse({src}, &{dst}, error, {path})) return false;")
        elif isinstance(t, ExternalT):
            out.append(f"if (!::orange::outputs::gen::{t.namespace}::parse({src}, &{dst}, error, {path})) return false;")
        elif isinstance(t, OneOfT):
            out.append("{")
            out.append("    int matches = 0;")
            out.append("    std::string reasons;")
            for i, branch in enumerate(t.branches):
                call = (f"::orange::outputs::gen::{branch.namespace}::parse" if isinstance(branch, ExternalT)
                        else "parse")
                out.append(f"    {{ {cpp_type(branch)} alternative{{}}; std::string why;")
                out.append(f"      if ({call}({src}, &alternative, &why, {path})) {{ {dst} = std::move(alternative); ++matches; }}")
                out.append(f"      else {{ reasons += \" [{i}] \" + why; }} }}")
            out.append(f"    if (matches != 1) return detail::fail(error, {path} + \": must match exactly one of "
                       f"{len(t.branches)} alternatives, matched \" + std::to_string(matches) + reasons);")
            out.append("}")
        elif isinstance(t, OpaqueT):
            if t.types:
                checks = {"object": "is_object", "array": "is_array", "string": "is_string",
                          "number": "is_number", "integer": "is_number_integer",
                          "boolean": "is_boolean", "null": "is_null"}
                condition = " || ".join(f"{src}.{checks[k]}()" for k in t.types)
                out.append(f"if (!({condition})) {fail('expected ' + ' or '.join(t.types))}")
            out.append(f"{dst} = {src};")
        elif isinstance(t, ArrayT):
            out.append(f"if (!{src}.is_array()) {fail('expected an array')}")
            out.append(f"{dst}.clear();")
            out.append(f"for (std::size_t i = 0; i < {src}.size(); ++i) {{")
            out.append(f"    {cpp_type(t.item)} item{{}};")
            out.append(f"    const std::string item_path = {path} + \"[\" + std::to_string(i) + \"]\";")
            out += ["    " + s for s in self.parse_stmt(t.item, f"{src}[i]", "item", "item_path", indent)]
            out.append(f"    {dst}.push_back(std::move(item));")
            out.append("}")
        elif isinstance(t, MapT):
            out.append(f"if (!{src}.is_object()) {fail('expected an object')}")
            out.append(f"{dst}.clear();")
            out.append(f"for (auto it = {src}.begin(); it != {src}.end(); ++it) {{")
            out.append(f"    {cpp_type(t.value)} value{{}};")
            out.append(f"    const std::string value_path = {path} + \"/\" + it.key();")
            if t.key_pattern is not None:
                self.regex_count += 1
                out.append(f"    static const std::regex key_pattern_{self.regex_count}({json.dumps(t.key_pattern)}, std::regex::ECMAScript);")
                out.append(f"    if (!std::regex_search(it.key(), key_pattern_{self.regex_count})) "
                           f"{{ return detail::fail(error, value_path + \": key does not match pattern\"); }}")
            out += ["    " + s for s in self.parse_stmt(t.value, "it.value()", "value", "value_path", indent)]
            out.append(f"    {dst}.emplace(it.key(), std::move(value));")
            out.append("}")
        return out

    def emit_object(self, obj: ObjectT) -> None:
        self.w(f"struct {obj.name} {{")
        for f in obj.fields:
            if f.is_const:
                if not f.required:
                    self.w(f"    bool has_{f.cpp_name}{{}};          // the optional const key is present")
                continue
            t = cpp_type(f.type)
            if f.nullable:
                t = f"std::optional<{t}>"          # nullopt: the value is null
            if not f.required:
                t = f"std::optional<{t}>"          # nullopt: the key is absent
            self.w(f"    {t} {f.cpp_name}{{}};")
        if obj.open:
            self.w("    // Keys the schema does not declare (the object is open), kept verbatim.")
            self.w("    nlohmann::json extra = nlohmann::json::object();")
        self.w("};")
        self.w()

    def emit_functions(self, obj: ObjectT) -> None:
        self.w(f"inline nlohmann::json to_json(const {obj.name}& value) {{")
        self.w("    (void)value;")
        self.w("    nlohmann::json out = nlohmann::json::object();")
        if obj.open:
            self.w("    for (auto it = value.extra.begin(); it != value.extra.end(); ++it) out[it.key()] = it.value();")
        for f in obj.fields:
            key = json.dumps(f.json_name)
            if f.is_const and not f.required:
                self.w(f"    if (value.has_{f.cpp_name}) out[{key}] = {json_literal(f.const)};")
            elif f.is_const:
                self.w(f"    out[{key}] = {json_literal(f.const)};")
            elif f.nullable and not f.required:
                self.w(f"    if (value.{f.cpp_name}) out[{key}] = *value.{f.cpp_name} ? {self.write_expr(f.type, '**value.' + f.cpp_name)} : nlohmann::json(nullptr);")
            elif f.nullable:
                self.w(f"    out[{key}] = value.{f.cpp_name} ? {self.write_expr(f.type, '*value.' + f.cpp_name)} : nlohmann::json(nullptr);")
            elif not f.required:
                self.w(f"    if (value.{f.cpp_name}) out[{key}] = {self.write_expr(f.type, '*value.' + f.cpp_name)};")
            else:
                self.w(f"    out[{key}] = {self.write_expr(f.type, 'value.' + f.cpp_name)};")
        self.w("    return out;")
        self.w("}")
        self.w()
        known = ", ".join(json.dumps(f.json_name) for f in obj.fields)
        self.w(f"inline bool parse(const nlohmann::json& in, {obj.name}* out, std::string* error,")
        self.w(f"                  const std::string& path = \"\") {{")
        self.w("    (void)out;")
        self.w(f"    if (!in.is_object()) return detail::fail(error, path + \": expected an object\");")
        self.w(f"    static const std::set<std::string> known{{{known}}};")
        if obj.open:
            self.w("    out->extra = nlohmann::json::object();")
        self.w("    for (auto it = in.begin(); it != in.end(); ++it) {")
        if obj.open:
            self.w("        if (!known.count(it.key())) out->extra[it.key()] = it.value();")
        else:
            self.w("        if (!known.count(it.key())) return detail::fail(error, path + \"/\" + it.key() + \": unexpected field\");")
        self.w("    }")
        for f in obj.fields:
            key = json.dumps(f.json_name)
            fpath = f"(path + \"/{f.json_name}\")"
            if f.is_const:
                cond = f"in.contains({key})"
                check = f"if (in.at({key}) != {json_literal(f.const)}) return detail::fail(error, {fpath} + \": must be \" + {json_literal(f.const)}.dump());"
                if f.required:
                    self.w(f"    if (!{cond}) return detail::fail(error, {fpath} + \": missing\");")
                    self.w(f"    {check}")
                else:
                    self.w(f"    out->has_{f.cpp_name} = {cond};")
                    self.w(f"    if ({cond}) {{ {check} }}")
                continue
            self.w(f"    if (in.contains({key})) {{")
            src = f"in.at({key})"
            if f.nullable:
                null_value = "std::nullopt" if f.required else f"std::optional<{cpp_type(f.type)}>{{}}"
                self.w(f"        if ({src}.is_null()) {{ out->{f.cpp_name} = {null_value}; }} else {{")
                self.w(f"            {cpp_type(f.type)} v{{}};")
                for s in self.parse_stmt(f.type, src, "v", fpath, ""):
                    self.w(f"            {s}")
                self.w(f"            out->{f.cpp_name} = std::move(v);")
                self.w("        }")
            elif not f.required:
                self.w(f"        {cpp_type(f.type)} v{{}};")
                for s in self.parse_stmt(f.type, src, "v", fpath, ""):
                    self.w(f"        {s}")
                self.w(f"        out->{f.cpp_name} = std::move(v);")
            else:
                for s in self.parse_stmt(f.type, src, f"out->{f.cpp_name}", fpath, ""):
                    self.w(f"        {s}")
            if f.required:
                self.w(f"    }} else {{ return detail::fail(error, {fpath} + \": missing\"); }}")
            else:
                self.w(f"    }} else {{ out->{f.cpp_name}.reset(); }}")
        for condition in obj.conditions:
            self.emit_condition(condition)
        self.w("    return true;")
        self.w("}")
        self.w()

    def rule_test(self, rule: Rule) -> list[tuple[str, str]]:
        """(C++ test on `const nlohmann::json& v`, failure text) per check of a leaf rule."""
        tests = []
        for keyword, value in rule.checks.items():
            if keyword == "const":
                tests.append((f"v == {json_literal(value)}", f"must be {json.dumps(value)}"))
            elif keyword == "enum":
                alternatives = " || ".join(f"v == {json_literal(x)}" for x in value)
                tests.append((f"({alternatives})", f"must be one of {json.dumps(value)}"))
            elif keyword == "type":
                checks = {"object": "is_object", "array": "is_array", "string": "is_string",
                          "number": "is_number", "integer": "is_number_integer",
                          "boolean": "is_boolean", "null": "is_null"}
                tests.append((f"v.{checks[value]}()", f"must be of type {value}"))
            elif keyword == "minLength":
                tests.append((f"!v.is_string() || v.get_ref<const std::string&>().size() >= {int(value)}u",
                              f"must be at least {value} characters"))
            elif keyword == "maxLength":
                tests.append((f"!v.is_string() || v.get_ref<const std::string&>().size() <= {int(value)}u",
                              f"must be at most {value} characters"))
            elif keyword == "pattern":
                self.regex_count += 1
                name = f"condition_pattern_{self.regex_count}"
                self.w(f"    static const std::regex {name}({json.dumps(value)}, std::regex::ECMAScript);")
                tests.append((f"!v.is_string() || std::regex_search(v.get_ref<const std::string&>(), {name})",
                              f"must match {value}"))
            elif keyword == "minimum":
                tests.append((f"!v.is_number() || v.get<double>() >= {float(value)!r}", f"must be >= {value}"))
            elif keyword == "maximum":
                tests.append((f"!v.is_number() || v.get<double>() <= {float(value)!r}", f"must be <= {value}"))
            elif keyword == "required":
                present = " && ".join(f"v.contains({json.dumps(k)})" for k in value)
                tests.append((f"!v.is_object() || ({present})", f"must contain {', '.join(value)}"))
            elif keyword == "forbidden":
                absent = " && ".join(f"!v.contains({json.dumps(k)})" for k in value)
                tests.append((f"!v.is_object() || ({absent})", f"must not contain {', '.join(value)}"))
        return tests

    def emit_condition(self, condition: Condition) -> None:
        """allOf[i] {if, then}: absent values satisfy `if` and are not constrained by `then`."""
        keys = lambda rule: "{" + ", ".join(json.dumps(k) for k in rule.path) + "}"
        when = []
        for rule in condition.when:
            test = " && ".join(f"({t})" for t, _ in self.rule_test(rule))
            when.append(f"[&] {{ const nlohmann::json* p = detail::at_path(in, {keys(rule)}); "
                        f"if (!p) return true; const nlohmann::json& v = *p; return {test}; }}()")
        self.w(f"    // allOf[{condition.index}]")
        self.w(f"    if ({' && '.join(when)}) {{")
        self.emit_rules(condition.then, condition.index, "")
        if condition.otherwise:
            self.w("    } else {")
            self.emit_rules(condition.otherwise, condition.index, " (else)")
        self.w("    }")

    def emit_rules(self, rules: list, index: int, branch: str) -> None:
        keys = lambda rule: "{" + ", ".join(json.dumps(k) for k in rule.path) + "}"
        for rule in rules:
            pointer = "".join("/" + k for k in rule.path)
            self.w(f"        if (const nlohmann::json* p = detail::at_path(in, {keys(rule)})) {{")
            self.w("            const nlohmann::json& v = *p;")
            for test, message in self.rule_test(rule):
                text = json.dumps(f"{pointer}: {message} (allOf[{index}]{branch})")
                self.w(f"            if (!({test})) return detail::fail(error, path + {text});")
            self.w("        }")


def generate(schema_path: Path, namespace: str, refs: dict | None = None) -> str:
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    root_name = schema.get("x-orange-cpp-name") or camel(schema_path.name.replace(".schema.json", ""))
    builder = Builder(schema, schema_path.parent, refs)
    top = builder.build(root_name)
    em = Emitter()
    em.w("// GENERATED by tools/schema_codegen/codegen.py from")
    em.w(f"// docs/schemas/{schema_path.name}. Do not edit; change the schema.")
    em.w("#pragma once")
    em.w()
    for inc in ("json.hpp", ):
        em.w(f'#include "{inc}"')
    for stem in sorted(builder.externals):
        em.w(f'#include "orange_outputs/{stem}.h"')
    for inc in ("cmath", "cstdint", "initializer_list", "map", "optional", "regex", "set",
                "string", "variant", "vector"):
        em.w(f"#include <{inc}>")
    em.w()
    em.w(f"namespace orange::outputs::gen::{namespace} {{")
    em.w()
    em.w("namespace detail {")
    em.w("inline bool fail(std::string* error, const std::string& message) { if (error) *error = message; return false; }")
    em.w("inline double round_to(double value, int decimals) {")
    em.w("    if (!std::isfinite(value)) return value;")
    em.w("    double scale = 1.0;")
    em.w("    for (int i = 0; i < decimals; ++i) scale *= 10.0;")
    em.w("    return std::round(value * scale) / scale;")
    em.w("}")
    em.w("// The value at a property path, or nullptr where a step is absent or not an object.")
    em.w("inline const nlohmann::json* at_path(const nlohmann::json& in, std::initializer_list<const char*> keys) {")
    em.w("    const nlohmann::json* node = &in;")
    em.w("    for (const char* key : keys) {")
    em.w("        if (!node->is_object()) return nullptr;")
    em.w("        const auto it = node->find(key);")
    em.w("        if (it == node->end()) return nullptr;")
    em.w("        node = &*it;")
    em.w("    }")
    em.w("    return node;")
    em.w("}")
    em.w("inline bool one_of(const std::string& value, std::initializer_list<const char*> allowed) {")
    em.w("    for (const char* a : allowed) if (value == a) return true;")
    em.w("    return false;")
    em.w("}")
    em.w("}  // namespace detail")
    em.w()
    em.w(f"inline constexpr const char* kSchemaFile = {json.dumps(schema_path.name)};")
    em.w()
    for obj in builder.objects:
        em.emit_object(obj)
    for obj in builder.objects:
        em.emit_functions(obj)
    if isinstance(top, VariantT):
        names = ", ".join(o.name for _, o in top.branches)
        em.w(f"using {top.name} = std::variant<{names}>;")
        em.w()
        em.w(f"inline nlohmann::json to_json(const {top.name}& value) {{")
        em.w("    return std::visit([](const auto& branch) { return to_json(branch); }, value);")
        em.w("}")
        em.w()
        em.w(f"inline bool parse(const nlohmann::json& in, {top.name}* out, std::string* error,")
        em.w("                  const std::string& path = \"\") {")
        em.w(f"    if (!in.is_object() || !in.contains({json.dumps(top.discriminator)}))")
        em.w(f"        return detail::fail(error, path + \": missing {top.discriminator}\");")
        em.w(f"    const nlohmann::json& kind = in.at({json.dumps(top.discriminator)});")
        for value, obj in top.branches:
            em.w(f"    if (kind == {json_literal(value)}) {{ {obj.name} branch; if (!parse(in, &branch, error, path)) return false; *out = std::move(branch); return true; }}")
        em.w(f"    return detail::fail(error, path + \"/{top.discriminator}: unknown value \" + kind.dump());")
        em.w("}")
        em.w()
    em.w(f"}}  // namespace orange::outputs::gen::{namespace}")
    return "\n".join(em.lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("schema", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--namespace", required=True)
    parser.add_argument("--ref", action="append", default=[],
                        help="SCHEMA_STEM=NAMESPACE of another generated schema this one may $ref")
    args = parser.parse_args()
    refs = {}
    for item in args.ref:
        stem, _, ns = item.partition("=")
        if not stem or not ns:
            print(f"codegen: --ref {item!r} is not SCHEMA_STEM=NAMESPACE", file=sys.stderr)
            return 2
        refs[stem] = ns
    try:
        text = generate(args.schema, args.namespace, refs)
    except SchemaError as exc:
        print(f"codegen: {args.schema.name}: {exc}", file=sys.stderr)
        return 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != text:
        args.output.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
