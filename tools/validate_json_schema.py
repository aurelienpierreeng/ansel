#!/usr/bin/python3
#   This file is part of darktable,
#   Copyright (C) 2026 Ansel contributors.
#
#   darktable is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   darktable is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
#
# Validate a JSON instance file against a JSON schema file.
#
# Replaces the `jsonschema` command-line tool, which the jsonschema library
# itself deprecated in favor of the separate `check-jsonschema` package
# (not packaged for every distro we build on). Calling the validation API
# directly avoids the CLI's deprecation warning and the extra dependency.

import json
import os
import sys


def _validated_path(path_str):
    base_dir = os.path.realpath(os.getcwd())
    path = os.path.realpath(os.path.join(base_dir, path_str))
    try:
        confined = os.path.commonpath([path, base_dir]) == base_dir
    except ValueError:
        # raised on Windows when the two paths are on different drives
        confined = False
    if not confined:
        print(f"error: path escapes the working directory: {path_str}", file=sys.stderr)
        sys.exit(2)
    if not os.path.isfile(path):
        print(f"error: not a file: {path_str}", file=sys.stderr)
        sys.exit(2)
    return path


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <instance.json> <schema.json>", file=sys.stderr)
        return 2

    import jsonschema

    instance_path = _validated_path(sys.argv[1])
    schema_path = _validated_path(sys.argv[2])

    with open(instance_path, encoding="utf-8") as f:
        instance = json.load(f)
    with open(schema_path, encoding="utf-8") as f:
        schema = json.load(f)

    validator_cls = jsonschema.validators.validator_for(schema)
    validator_cls.check_schema(schema)
    validator = validator_cls(schema)

    errors = sorted(validator.iter_errors(instance), key=str)
    for error in errors:
        print(error, file=sys.stderr)

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
