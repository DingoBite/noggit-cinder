import json
import os
from pathlib import Path
import socketserver
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

import noggit_mcp


SERVER = Path(__file__).with_name("noggit_mcp.py")
TEST_TOKEN = "a1" * 32


def request(request_id, method, **params):
    return {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}


def run_server(messages, **environment):
    lines = [item if isinstance(item, str) else json.dumps(item) for item in messages]
    completed = subprocess.run(
        [sys.executable, str(SERVER)],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        env={**os.environ, **environment},
        timeout=10,
        check=True,
    )
    return [json.loads(line) for line in completed.stdout.splitlines()]


class BridgeTest(unittest.TestCase):
    def test_initialize_discover_and_reject_invalid_arguments(self):
        responses = run_server([
            request(-1, "server/discover"),
            request(0, "tools/list"),
            request(1, "initialize", protocolVersion="2025-06-18"),
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            request(2, "tools/list"),
            request(3, "tools/call", name="noggit_open_map", arguments={"map_id": 1, "tile_x": 64, "tile_y": 0}),
            request(4, "tools/call", name="noggit_open_map", arguments={"map_id": True, "tile_x": 1, "tile_y": 0}),
            request(5, "tools/call", name="noggit_unknown"),
            "bad json",
            request(6, "ping"),
        ])
        self.assertEqual(responses.pop(0)["error"]["code"], -32601)
        self.assertEqual(responses[0]["error"]["code"], -32002)
        self.assertEqual(responses[1]["result"]["protocolVersion"], "2025-06-18")
        self.assertEqual(
            {tool["name"] for tool in responses[2]["result"]["tools"]},
            {"noggit_status", "noggit_open_project", "noggit_list_maps", "noggit_open_map"},
        )
        self.assertIn("tile_x", responses[3]["result"]["content"][0]["text"])
        self.assertTrue(responses[3]["result"]["isError"])
        self.assertIn("map_id", responses[4]["result"]["content"][0]["text"])
        self.assertEqual(responses[5]["error"]["code"], -32602)
        self.assertEqual(responses[6]["error"]["code"], -32700)
        self.assertEqual(responses[7]["result"], {})

    def test_round_trip_with_editor(self):
        received = []

        class Editor(socketserver.StreamRequestHandler):
            def handle(self):
                request = json.loads(self.rfile.readline())
                received.append(request)
                if request["method"] == "open_map":
                    response = {"error": "This tile does not exist."}
                elif request["method"] == "open_project":
                    response = {"result": {"state": "map_selection", "project": "Нордскол"}}
                else:
                    response = {"result": {"state": "project_selection"}}
                self.wfile.write(json.dumps(response, ensure_ascii=False).encode("utf-8") + b"\n")

        with socketserver.TCPServer(("127.0.0.1", 0), Editor) as editor:
            thread = threading.Thread(target=editor.serve_forever, daemon=True)
            thread.start()
            try:
                responses = run_server([
                    request(1, "initialize", protocolVersion="2025-11-25"),
                    request(2, "tools/call", name="noggit_status"),
                    request(3, "tools/call", name="noggit_open_project", arguments={"project_path": "Нордскол\\Map.noggitproj"}),
                    request(4, "tools/call", name="noggit_open_map", arguments={"map_id": 1, "tile_x": 2, "tile_y": 3}),
                ], NOGGIT_MCP_TOKEN=TEST_TOKEN, NOGGIT_MCP_PORT=str(editor.server_address[1]))
            finally:
                editor.shutdown()
                thread.join()

        self.assertEqual(received, [
            {"token": TEST_TOKEN, "method": "status", "arguments": {}},
            {"token": TEST_TOKEN, "method": "open_project", "arguments": {"project_path": "Нордскол/Map.noggitproj"}},
            {"token": TEST_TOKEN, "method": "open_map", "arguments": {"map_id": 1, "tile_x": 2, "tile_y": 3}},
        ])
        self.assertEqual(responses[1]["result"]["structuredContent"]["state"], "project_selection")
        self.assertEqual(responses[2]["result"]["structuredContent"]["project"], "Нордскол")
        self.assertTrue(responses[3]["result"]["isError"])
        self.assertEqual(responses[3]["result"]["content"][0]["text"], "This tile does not exist.")
        self.assertNotIn(TEST_TOKEN, json.dumps(responses))

    def test_reject_project_paths_outside_projects_folder(self):
        invalid_paths = [
            "../foo.noggitproj", "foo\\..\\bar.noggitproj", "C:foo.noggitproj",
            "C:/foo.noggitproj", "/foo.noggitproj", "\\\\localhost\\share\\foo.noggitproj",
            "foo/.. /bar.noggitproj", "foo\x00.noggitproj", "foo.json", "a" * 512 + ".noggitproj",
        ]
        responses = run_server([
            request(1, "initialize", protocolVersion="2025-11-25"),
            *(request(index, "tools/call", name="noggit_open_project", arguments={"project_path": path})
              for index, path in enumerate(invalid_paths, start=2)),
        ], NOGGIT_MCP_TOKEN="")
        for response in responses[1:]:
            self.assertTrue(response["result"]["isError"])
            self.assertIn("project_path", response["result"]["content"][0]["text"])

    def test_missing_configuration_returns_tool_error(self):
        responses = run_server([
            request(1, "initialize", protocolVersion="unknown"),
            request(2, "tools/call", name="noggit_list_maps"),
        ], NOGGIT_MCP_TOKEN="")
        self.assertEqual(responses[0]["result"]["protocolVersion"], "2025-11-25")
        self.assertTrue(responses[1]["result"]["isError"])
        self.assertIn("token", responses[1]["result"]["content"][0]["text"])

    def test_connection_file_is_reloaded_and_env_can_override_it(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "connection.json"
            with patch.object(noggit_mcp, "connection_path", return_value=config), patch.dict(os.environ, {}, clear=True):
                with self.assertRaisesRegex(ValueError, "Start noggit.exe"):
                    noggit_mcp.connection_settings()
                config.write_text(json.dumps({"token": TEST_TOKEN, "port": 38556}), encoding="utf-8")
                self.assertEqual(noggit_mcp.connection_settings(), (TEST_TOKEN, 38556))
                config.write_text(json.dumps({"token": "b2" * 32, "port": 38557}), encoding="utf-8")
                self.assertEqual(noggit_mcp.connection_settings(), ("b2" * 32, 38557))
                with patch.dict(os.environ, {"NOGGIT_MCP_PORT": "38558"}):
                    self.assertEqual(noggit_mcp.connection_settings(), ("b2" * 32, 38558))
                config.write_text("invalid JSON", encoding="utf-8")
                with patch.dict(os.environ, {"NOGGIT_MCP_TOKEN": TEST_TOKEN}):
                    self.assertEqual(noggit_mcp.connection_settings(), (TEST_TOKEN, 38555))

    def test_malformed_connection_file_fails_without_disclosing_token(self):
        invalid_connections = [
            "invalid JSON", "[]", json.dumps({"token": TEST_TOKEN}),
            json.dumps({"token": TEST_TOKEN, "port": True}),
            json.dumps({"token": TEST_TOKEN, "port": "38555"}),
            json.dumps({"token": TEST_TOKEN, "port": 65536}),
            json.dumps({"token": "bad token", "port": 38555}),
        ]
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "connection.json"
            with patch.object(noggit_mcp, "connection_path", return_value=config), patch.dict(os.environ, {}, clear=True):
                for data in invalid_connections:
                    with self.subTest(data=data):
                        config.write_text(data, encoding="utf-8")
                        result = noggit_mcp.call_tool("status", {}, "2025-11-25")
                        self.assertTrue(result["isError"])
                        self.assertNotIn(TEST_TOKEN, json.dumps(result))


if __name__ == "__main__":
    unittest.main()
