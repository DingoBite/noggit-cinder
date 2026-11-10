"""Stdio MCP server for the local loopback bridge in Noggit Cinder."""

import json
import os
from pathlib import Path
import re
import socket
import sys
import time


PROTOCOL_VERSIONS = ("2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05")
MAX_REQUEST = 1024 * 1024
MAX_EDITOR_RESPONSE = 4 * 1024 * 1024
EDITOR_PORT = 38555
EDITOR_TIMEOUT = 30

TOOLS = {
    "status": {
        "description": "Get the active Noggit Cinder project and editor state.",
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
        "properties": {},
    },
    "open_project": {
        "description": "Open an existing Noggit project from the project selection screen.",
        "annotations": {"destructiveHint": False, "openWorldHint": False},
        "properties": {
            "project_path": {
                "type": "string",
                "minLength": 1,
                "maxLength": 512,
                "description": "Path to a .noggitproj relative to Noggit's projects folder, e.g. WotLK/WotLK.noggitproj.",
            },
        },
    },
    "list_maps": {
        "description": "List the available maps in the active Noggit project and their map IDs.",
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
        "properties": {},
    },
    "open_map": {
        "description": "Open an existing terrain tile or a WMO-only map in Noggit. "
        "Get the map ID with noggit_list_maps and use the map selection screen to switch maps. "
        "For WMO-only maps, tile coordinates are ignored. This preserves map edits.",
        "annotations": {"destructiveHint": False, "openWorldHint": False},
        "properties": {
            "map_id": {"type": "integer", "minimum": 0, "maximum": 2147483647},
            "tile_x": {"type": "integer", "minimum": 0, "maximum": 63},
            "tile_y": {"type": "integer", "minimum": 0, "maximum": 63},
        },
    },
}


def tool_list():
    return [
        {
            "name": f"noggit_{method}",
            "description": tool["description"],
            "annotations": tool["annotations"],
            "inputSchema": {
                "type": "object",
                "properties": tool["properties"],
                "required": list(tool["properties"]),
                "additionalProperties": False,
            },
        }
        for method, tool in TOOLS.items()
    ]


def decode_message(data):
    return json.loads(data.decode("utf-8"))


def read_editor_response(connection):
    deadline = time.monotonic() + EDITOR_TIMEOUT
    response = bytearray()
    while len(response) < MAX_EDITOR_RESPONSE:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError
        connection.settimeout(remaining)
        chunk = connection.recv(min(65536, MAX_EDITOR_RESPONSE - len(response)))
        if not chunk:
            break
        response.extend(chunk)
        if b"\n" in chunk:
            return response.partition(b"\n")[0]
    raise ValueError("Noggit sent an incomplete or oversized response.")


def connection_path():
    if sys.platform == "win32":
        root = os.environ.get("LOCALAPPDATA")
        if not root:
            raise ValueError("LOCALAPPDATA is unavailable for the Noggit MCP connection.")
    elif sys.platform == "darwin":
        root = Path.home() / "Library" / "Preferences"
    else:
        root = os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config"
    return Path(root) / "NoggitCinderMCP" / "connection.json"


def connection_settings():
    if "NOGGIT_MCP_TOKEN" in os.environ:
        token, port = os.environ["NOGGIT_MCP_TOKEN"], EDITOR_PORT
    else:
        try:
            with connection_path().open("rb") as config:
                data = config.read(4097)
            if len(data) > 4096:
                raise ValueError
            connection = json.loads(data)
            if not isinstance(connection, dict):
                raise ValueError
            token, port = connection.get("token"), connection.get("port")
        except FileNotFoundError as error:
            raise ValueError("Start noggit.exe once to create the local MCP connection.") from error
        except (OSError, ValueError, RecursionError) as error:
            raise ValueError("Could not read a valid local Noggit MCP connection file.") from error

    if not isinstance(token, str) or not re.fullmatch(r"[0-9a-fA-F]{32,128}", token):
        raise ValueError("The Noggit MCP token must contain 32–128 hexadecimal characters.")
    if "NOGGIT_MCP_PORT" in os.environ:
        try:
            port = int(os.environ["NOGGIT_MCP_PORT"])
        except ValueError:
            port = 0
    if type(port) is not int or not 1 <= port <= 65535:
        raise ValueError("NOGGIT_MCP_PORT must be an integer between 1 and 65535.")
    return token, port


def send_to_editor(method, arguments):
    token, port = connection_settings()
    message = json.dumps({"token": token, "method": method, "arguments": arguments})
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=EDITOR_TIMEOUT) as connection:
            connection.sendall(message.encode("utf-8") + b"\n")
            response = read_editor_response(connection)
    except TimeoutError as error:
        raise ValueError("Noggit did not reply in time. Check the editor window before retrying.") from error
    except OSError as error:
        raise ValueError("Could not reach Noggit. Start the MCP-enabled build with the matching port.") from error

    try:
        response = decode_message(response)
    except (ValueError, RecursionError) as error:
        raise ValueError("Noggit sent an invalid response.") from error
    if isinstance(response, dict):
        if isinstance(response.get("error"), str):
            raise ValueError(response["error"])
        if isinstance(response.get("result"), dict):
            return response["result"]
    raise ValueError("Noggit sent an invalid response.")


def project_relative_path(value):
    message = "project_path must be a .noggitproj path relative to Noggit's projects folder (up to 512 characters)."
    if not isinstance(value, str) or len(value) > 512:
        raise ValueError(message)
    path = value.replace("\\", "/")
    if (
        not path.lower().endswith(".noggitproj")
        or re.search(r'[\x00-\x1f<>:"|?*]', path)
        or any(not part or part.endswith((".", " ")) for part in path.split("/"))
    ):
        raise ValueError(message)
    return path


def call_tool(method, arguments, protocol_version):
    properties = TOOLS[method]["properties"]
    try:
        if arguments.keys() != properties.keys():
            expected = ", ".join(properties) or "no arguments"
            raise ValueError(f"Expected {expected}.")
        if method == "open_project":
            arguments = {"project_path": project_relative_path(arguments["project_path"])}
        else:
            for name, value in arguments.items():
                minimum, maximum = properties[name]["minimum"], properties[name]["maximum"]
                if type(value) is not int or not minimum <= value <= maximum:
                    raise ValueError(f"{name} must be an integer between {minimum} and {maximum}.")
        result = send_to_editor(method, arguments)
    except ValueError as error:
        return {"content": [{"type": "text", "text": str(error)}], "isError": True}

    response = {"content": [{"type": "text", "text": json.dumps(result, ensure_ascii=False)}]}
    if protocol_version >= "2025-06-18":
        response["structuredContent"] = result
    return response


def serve(stdin, stdout):
    protocol_version = None

    def send(request_id, **payload):
        response = {"jsonrpc": "2.0", "id": request_id, **payload}
        stdout.write(json.dumps(response).encode("utf-8") + b"\n")
        stdout.flush()

    def fail(request_id, code, message):
        send(request_id, error={"code": code, "message": message})

    while line := stdin.readline(MAX_REQUEST + 1):
        if len(line) > MAX_REQUEST:
            fail(None, -32700, "Request exceeds the maximum frame size.")
            break
        try:
            request = decode_message(line)
        except (ValueError, RecursionError):
            fail(None, -32700, "Invalid JSON.")
            continue
        if (
            not isinstance(request, dict)
            or request.get("jsonrpc") != "2.0"
            or not isinstance(request.get("method"), str)
            or ("id" in request and type(request["id"]) not in (str, int))
        ):
            fail(None, -32600, "Invalid request.")
            continue
        if "id" not in request:
            continue

        request_id, method = request["id"], request["method"]
        params = request.get("params", {})
        if not isinstance(params, dict):
            fail(request_id, -32602, "Expected an object for params.")
        elif method == "initialize":
            version = params.get("protocolVersion")
            if not isinstance(version, str):
                fail(request_id, -32602, "Expected a protocolVersion string.")
                continue
            protocol_version = version if version in PROTOCOL_VERSIONS else PROTOCOL_VERSIONS[0]
            send(request_id, result={
                "protocolVersion": protocol_version,
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "noggit-cinder", "version": "0.1.0"},
            })
        elif method == "ping":
            send(request_id, result={})
        elif method not in ("tools/list", "tools/call"):
            fail(request_id, -32601, "Method not found.")
        elif protocol_version is None:
            fail(request_id, -32002, "Initialize the MCP connection first.")
        elif method == "tools/list":
            send(request_id, result={"tools": tool_list()})
        else:
            name, arguments = params.get("name"), params.get("arguments", {})
            if not isinstance(name, str) or not isinstance(arguments, dict):
                fail(request_id, -32602, "Expected a tool name and an arguments object.")
            elif not name.startswith("noggit_") or name[7:] not in TOOLS:
                fail(request_id, -32602, "Unknown Noggit tool.")
            else:
                send(request_id, result=call_tool(name[7:], arguments, protocol_version))


if __name__ == "__main__":
    serve(sys.stdin.buffer, sys.stdout.buffer)
