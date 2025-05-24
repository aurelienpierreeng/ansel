#!/usr/bin/env python3
# This script is part of the Ansel project
# Copyright (c) 2025 - Aurélien Pierre
#
# Scan C/C++ header files in the src directory for function declarations
# and check if they are used in the source files.
#
# WARNING: some functions will appear unused from .c files but may be used in
# generated code or XML/XSL templates, in particular for the preferences popup.
# Unused functions may be unneeded (if API/logic changed), or they might have been
# forgotten or not yet implemented, in which case they will be useful in the future.
# Don't remove them without checking which it is.
#
# Ensure to check the function name in the code before removing it.
#
# Call this script from the root of the repository.
#
# Usage:
#   - python3 tools/detect-unused-functions.py -> scan all header files
#   - python3 tools/detect-unused-functions.py <header_file> -> scan a specific header file


import os
import re
import sys

# Parse optional argument for a specific header file
header_file_arg = None
if len(sys.argv) > 1:
  header_file_arg = sys.argv[1]

excluded_dirs = [
  "external",
  "tests"
]

excluded_files = [
  "paint.h"
]

def find_files_with_extensions(directory, extensions):
  for root, _, files in os.walk(directory):
    # Skip any directory under ./external
    if not any(excluded in root for excluded in excluded_dirs):
      for file in files:
        if header_file_arg is not None:
          if file == header_file_arg:
            yield os.path.join(root, file)
        elif any(file.endswith(ext) for ext in extensions) and not any(excluded in file for excluded in excluded_files):
            yield os.path.join(root, file)

def extract_function_declarations(file_path):
  # Regex to match C function declarations (not definitions), excluding control keywords
  # Matches: [qualifiers] [return type] function_name(
  pattern = re.compile(
    r'^\s*'                                 # Line start, optional whitespace
    r'(?!#define\b|if\b|else\b|return|typedef\b)'             # Exclude lines starting with if/else/return
    r'([a-zA-Z_][\w\s\*\(\)]*?)'            # Return type and qualifiers (non-greedy)
    r'\s+([a-zA-Z_]\w*)\s*\(',               # Function name before '('
    re.MULTILINE
  )
  declarations = []
  with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
    for line in f:
      match = pattern.match(line)
      if match:
        # Combine type/qualifier and function name
        type_qualifier = match.group(1).strip()
        func_name = match.group(2).strip()
        declarations.append(f"{type_qualifier} {func_name}")
  return declarations

def main():
  output_file = "function_declarations.txt"
  directory = os.path.join(os.getcwd(), "src")
  all_declarations = []
  for h_file in find_files_with_extensions(directory, [".h", ".hpp"]):
    all_declarations.extend(extract_function_declarations(h_file))
  with open(output_file, 'w') as out:
    for decl in all_declarations:
      out.write(decl + '\n')
  print(f"Extracted {len(all_declarations)} function declarations to {output_file}")

  for decl in all_declarations:
    qualifiers, func_name = decl.rsplit(' ', 1)

    # Default functions appear not used but are used in IOP/LIB API
    # through macros
    if func_name.startswith("default_"):
      continue

    used = 0
    matches = []
    # Use grep to find occurrences of func_name in .c and .h files, with file and line number
    grep_cmd = (
      f"grep -rnw --include='*.c' --include='*.h' --include='*.mm' --include='*.cpp' --include='*.cc' --include='*.hpp' "
      f". -e '{func_name}'"
    )
    try:
      grep_output = os.popen(grep_cmd).read().strip()
      if grep_output:
        matches = set(grep_output.split('\n'))
        used = len(matches)
    except Exception:
      used = 0
      matches = {}

    error = False
    if "static" in qualifiers:
      if used < 2:
        print(f"{qualifiers} {func_name}: STATIC NOT USED")
        error = True
    else:
      if used < 2:
        print(f"{qualifiers} {func_name}: NOT DEFINED")
        error = True
      elif used < 3:
        print(f"{qualifiers} {func_name}: NOT USED")
        error = True

    if error:
      [print(f"   {match}") for match in matches]


if __name__ == "__main__":
  main()
