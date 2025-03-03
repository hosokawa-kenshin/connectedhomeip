import json
import sys
import re

"""
Parses an output of chip-tool and converts it to a JSON format.

Functions:
    parse_log_to_json(log: str) -> str:
        Parses the given log string and converts it to a JSON formatted string.

Parameters:
    log (str): The log string to be parsed.

Returns:
    str: The JSON formatted string.

Usage:
    The script reads a log from standard input, parses it using the parse_log_to_json function,
    and prints the resulting JSON string.

Example:
    $ chip-tool onoff read on-off ${NODE_ID} ${ENDPOINT_ID} | python chip-parse.py
"""

def delete_garbage(log):
    log = re.sub(r'\x1b\[[0-9;]*m', '', log)
    log = re.sub(r',', '', log)

    row_lines = log.splitlines()
    lines = []
    parsed_json = ""

    for line in row_lines:
        line = line.strip()
        columns = line.split()
        if len(columns) >= 4 and any(columns[3:]):
            lines.append(line)

    for i, line in enumerate(lines):
        line = line.strip()
        columns = line.split()

        if not(len(columns) >= 3 and columns[2] == '[DMG]' and ('=' in line or '{' in line or '}' in line or columns[3:] == "[" or columns[3:] == "]")):
            lines.pop(i)
    
    return log

def parse_log_to_json(log):
    depth = 0
    log = re.sub(r'\x1b\[[0-9;]*m', '', log)
    log = re.sub(r',', '', log)
    row_lines = log.splitlines()
    lines = []
    parsed_json = ""

    for line in row_lines:
        line = line.strip()
        columns = line.split()
        if len(columns) >= 4 and any(columns[3:]):
            lines.append(line)

    parsed_json += '{'
    for i, line in enumerate(lines):
        line = line.strip()
        columns = line.split()
        if len(columns) >= 3 and columns[2] == '[DMG]':
            line = ' '.join(columns[3:])
            if '{' in line  or line == "[":
                depth += 1
                parsed_json += line
            elif '}' in line or line == "]" or line == "],":
                if depth > 0:
                    line = line.split(',')[0]
                    next_line = lines[i + 1].strip()
                    next_columns = next_line.split()
                    if len(next_columns) >= 3 and next_columns[2] == '[DMG]' and '=' in next_line:
                        line = f"{line},"
                    else:
                        line = f"{line}"
                    depth -= 1
                parsed_json += line
            elif '=' in line:
                line = line.replace('=', ':')
                subcolumns = line.split(':')
                if len(subcolumns) > 1:
                    left_side = subcolumns[0].strip()
                    right_side = subcolumns[1].strip()
                    if right_side.startswith('0x'):
                        right_side = right_side.split()[0].split(',')[0]
                        right_side = int(right_side, 16)
                    elif right_side.isdigit():
                        right_side = int(right_side)
                    if i + 1 < len(lines):
                        next_line = lines[i + 1].strip()
                        next_columns = next_line.split()
                        if len(next_columns) >= 3 and next_columns[2] == '[DMG]' and '=' in next_line:
                            line = f"{left_side}: {right_side},"
                        else:
                            line = f"{left_side}: {right_side}"
                parsed_json += line
    parsed_json += '}'
    return parsed_json

log = sys.stdin.read()
parsed_json = parse_log_to_json(log)
print(parsed_json)