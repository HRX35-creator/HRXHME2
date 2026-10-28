#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (c) 2025 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""A mock AI service that speaks the protocols used by the AI functions.

The competition committee provides a mock AI service that listens on
0.0.0.0:8000 and answers every model request.  This script is a local stand
in that speaks the very same protocols, it is used to reproduce the end to
end test ``t/ai_function_enhancement.test`` locally.

The answers never depend on the name of the model, they are derived from the
request only, so the service is a pure function of its input:

  * the completion answers with the prompt it received,
  * the embedding answers with a vector whose length is the requested
    dimension and whose values are derived from the text,
  * the rerank answers with one result per document, the score of a document
    is derived from its text.

Usage:

  python3 mock_ai_service.py [--host 0.0.0.0] [--port 8000]

Routes:

  POST /v1/chat/completions   openai compatible completion
  POST /v1/embeddings          openai compatible embeddings
  POST /v1/rerank              siliconflow compatible rerank
  POST /api/generate           ollama native completion
  POST /api/chat               ollama native chat, answered as /api/generate
  POST /api/embeddings         ollama native embeddings
  POST /api/rerank             dashscope compatible rerank

The service also keeps the number of the requests that it received, it makes
it possible to check that the rows of a table scan are sent to the model in
one request:

  GET /__requests                 the number of the requests per route
  GET /__requests/reset           resets the counters
"""

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# the dimension of the embedding when the request does not ask for one
DEFAULT_DIMENSION = 8


def _text_seed(text):
    """A stable number that is derived from the text."""
    seed = 0
    for index, char in enumerate(text):
        seed += (index + 1) * ord(char)
    return seed


def make_embedding(text, dimension):
    """Build the embedding of the text, it only depends on the text."""
    seed = _text_seed(text)
    values = []
    for step in range(dimension):
        values.append(round(((seed + step * 37) % 10000) / 10000.0, 6))
    return values


def make_score(text):
    """Build the relevance score of the document, it only depends on the text."""
    return round((_text_seed(text) % 1000) / 1000.0, 6)


def _get_dimension(request):
    """The dimension that the request asks for."""
    dimension = request.get("dimensions")
    if isinstance(dimension, bool) or not isinstance(dimension, int):
        return DEFAULT_DIMENSION
    if dimension <= 0:
        return DEFAULT_DIMENSION
    return dimension


def _as_text(value):
    if isinstance(value, str):
        return value
    if value is None:
        return ""
    return json.dumps(value, ensure_ascii=False, sort_keys=True)


def openai_completion(request):
    """The answer of the openai compatible completion."""
    messages = request.get("messages")
    prompt = ""
    if isinstance(messages, list) and messages:
        last = messages[-1]
        if isinstance(last, dict):
            prompt = _as_text(last.get("content"))
    if not prompt:
        prompt = _as_text(request.get("prompt"))
    model = request.get("model")
    if not isinstance(model, str):
        model = "mock-model"
    return {
        "id": "chatcmpl-mock",
        "object": "chat.completion",
        "created": 0,
        "model": model,
        "choices": [
            {
                "index": 0,
                "message": {"role": "assistant", "content": prompt},
                "finish_reason": "stop",
            }
        ],
        "usage": {
            "prompt_tokens": len(prompt.split()),
            "completion_tokens": len(prompt.split()),
            "total_tokens": 2 * len(prompt.split()),
        },
    }


def ollama_completion(request):
    """The answer of the ollama native completion."""
    prompt = _as_text(request.get("prompt"))
    model = request.get("model")
    if not isinstance(model, str):
        model = "mock-model"
    return {
        "model": model,
        "response": prompt,
        "done": True,
        "done_reason": "stop",
    }


def openai_embeddings(request):
    """The answer of the openai compatible embeddings."""
    raw_inputs = request.get("input")
    if isinstance(raw_inputs, str):
        inputs = [raw_inputs]
    elif isinstance(raw_inputs, list):
        inputs = [_as_text(item) for item in raw_inputs]
    else:
        inputs = []
    dimension = _get_dimension(request)
    model = request.get("model")
    if not isinstance(model, str):
        model = "mock-model"
    data = []
    for index, text in enumerate(inputs):
        data.append(
            {
                "object": "embedding",
                "index": index,
                "embedding": make_embedding(text, dimension),
            }
        )
    # the answers are sent back in the reverse order on purpose, the client
    # has to restore the order with the index of every answer
    data.reverse()
    return {
        "object": "list",
        "data": data,
        "model": model,
        "usage": {"prompt_tokens": len(inputs), "total_tokens": len(inputs)},
    }


def ollama_embeddings(request):
    """The answer of the ollama native embeddings."""
    raw_inputs = request.get("input")
    if isinstance(raw_inputs, str):
        inputs = [raw_inputs]
    elif isinstance(raw_inputs, list):
        inputs = [_as_text(item) for item in raw_inputs]
    else:
        inputs = []
    dimension = _get_dimension(request)
    model = request.get("model")
    if not isinstance(model, str):
        model = "mock-model"
    return {
        "model": model,
        "embeddings": [make_embedding(text, dimension) for text in inputs],
    }


def _rerank_results(request):
    """The results of a rerank, the score of a document only depends on it."""
    documents = request.get("documents")
    if not isinstance(documents, list):
        documents = []
    results = []
    for index, document in enumerate(documents):
        results.append(
            {"index": index, "relevance_score": make_score(_as_text(document))}
        )
    # the results are sent back with the smaller score first, the client has
    # to sort them with the bigger score first
    results.sort(key=lambda item: (item["relevance_score"], item["index"]))
    return results


def siliconflow_rerank(request):
    """The answer of the siliconflow compatible rerank."""
    return {"results": _rerank_results(request)}


def dashscope_rerank(request):
    """The answer of the dashscope compatible rerank."""
    return {"output": {"results": _rerank_results(request)}}


class MockAIHandler(BaseHTTPRequestHandler):
    """The request handler of the mock AI service."""

    protocol_version = "HTTP/1.1"
    # the number of the requests per route, it makes it possible to check
    # that the rows of a table scan are sent to the model in one request
    request_count = {}

    def log_message(self, fmt, *args):
        sys.stderr.write(
            "[mock-ai] %s - %s\n" % (self.address_string(), fmt % args)
        )

    def _count(self, route):
        MockAIHandler.request_count[route] = (
            MockAIHandler.request_count.get(route, 0) + 1
        )

    def _read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        try:
            return json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return {}

    def _send(self, payload, status=200):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/__requests":
            self._send(dict(MockAIHandler.request_count))
        elif self.path == "/__requests/reset":
            MockAIHandler.request_count = {}
            self._send({"status": "ok"})
        else:
            self._send({"error": "unknown route"}, status=404)

    def do_POST(self):
        route = self.path.split("?", 1)[0]
        request = self._read_body()
        self._count(route)
        if route in ("/v1/chat/completions",):
            self._send(openai_completion(request))
        elif route in ("/api/generate", "/api/chat"):
            self._send(ollama_completion(request))
        elif route in ("/v1/embeddings",):
            self._send(openai_embeddings(request))
        elif route in ("/api/embeddings",):
            self._send(ollama_embeddings(request))
        elif route in ("/v1/rerank",):
            self._send(siliconflow_rerank(request))
        elif route in ("/api/rerank",):
            self._send(dashscope_rerank(request))
        else:
            self._send({"error": "unknown route"}, status=404)


def selftest():
    """Print the answer of every route, it checks the service without a server."""
    cases = [
        ("/v1/chat/completions", openai_completion,
         {"model": "m", "messages": [{"role": "user", "content": "hello"}]}),
        ("/api/generate", ollama_completion,
         {"model": "m", "prompt": "hello", "stream": False}),
        ("/v1/embeddings", openai_embeddings,
         {"model": "m", "input": ["a", "b"], "dimensions": 4}),
        ("/api/embeddings", ollama_embeddings,
         {"model": "m", "input": ["a", "b"], "dimensions": 4}),
        ("/v1/rerank", siliconflow_rerank,
         {"model": "m", "query": "q", "documents": ["a", "b"]}),
        ("/api/rerank", dashscope_rerank,
         {"model": "m", "query": "q", "documents": ["a", "b"]}),
    ]
    for route, handler, request in cases:
        answer = handler(request)
        print("%s -> %s" % (route, json.dumps(answer, ensure_ascii=False)))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="mock AI service of seekdb")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--selftest", action="store_true",
                        help="print the answer of every route and exit")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    server = ThreadingHTTPServer((args.host, args.port), MockAIHandler)
    sys.stderr.write(
        "[mock-ai] listening on %s:%d\n" % (args.host, args.port)
    )
    sys.stderr.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
