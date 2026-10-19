#!/usr/bin/env python3
"""End-to-end smoke test over a real WebSocket connection.

Usage:  python3 tests/e2e_ws_test.py [ws://localhost:8080/ws]
Needs:  pip install websockets
Used by CI against the Docker image.
"""
import asyncio
import json
import sys

import websockets

URL = sys.argv[1] if len(sys.argv) > 1 else "ws://localhost:8080/ws"


async def next_of(ws, type_, pred=lambda m: True, timeout=5):
    async def loop():
        while True:
            m = json.loads(await ws.recv())
            if m.get("type") == type_ and pred(m):
                return m
    return await asyncio.wait_for(loop(), timeout)


async def main():
    async with websockets.connect(URL) as a, websockets.connect(URL) as b:
        hello = await next_of(a, "hello")
        assert "AAPL" in hello["symbols"], hello
        await next_of(a, "book", lambda m: m["symbol"] == "AAPL")
        print("ok  hello + initial book snapshot")

        # 1. A resting order far from the market, then cancel it.
        await a.send(json.dumps({"op": "new", "symbol": "AAPL", "side": "buy", "type": "limit",
                                 "price": 1.00, "qty": 5, "clOrdId": "r1"}))
        ack = await next_of(a, "ack", lambda m: m["clOrdId"] == "r1")
        assert ack["status"] == "resting" and ack["remaining"] == 5, ack
        await a.send(json.dumps({"op": "cancel", "symbol": "AAPL", "id": ack["id"]}))
        c = await next_of(a, "cancel_ack")
        assert c["ok"] is True, c
        print("ok  resting limit order + cancel")

        # 2. Client B cannot cancel client A's order.
        await a.send(json.dumps({"op": "new", "symbol": "AAPL", "side": "sell", "type": "limit",
                                 "price": 99999, "qty": 1, "clOrdId": "r2"}))
        ack2 = await next_of(a, "ack", lambda m: m["clOrdId"] == "r2")
        await b.send(json.dumps({"op": "cancel", "symbol": "AAPL", "id": ack2["id"]}))
        c2 = await next_of(b, "cancel_ack")
        assert c2["ok"] is False, c2
        print("ok  ownership check on cancel")

        # 3. Market order trades against the bots' liquidity and is broadcast.
        await b.send(json.dumps({"op": "new", "symbol": "AAPL", "side": "buy", "type": "market",
                                 "qty": 3, "clOrdId": "m1"}))
        ack3 = await next_of(b, "ack", lambda m: m["clOrdId"] == "m1")
        assert ack3["filled"] > 0, ack3
        await next_of(a, "trades", lambda m: any(t["buyId"] == ack3["id"] for t in m["trades"]))
        print("ok  market order filled, trade broadcast to other client")

        # 4. Validation errors.
        await a.send("not json")
        e = await next_of(a, "error")
        await a.send(json.dumps({"op": "new", "symbol": "NOPE", "side": "buy", "type": "limit",
                                 "price": 1, "qty": 1}))
        e2 = await next_of(a, "error")
        assert e["message"] == "invalid JSON" and e2["message"] == "unknown symbol", (e, e2)
        print("ok  input validation")

        await next_of(a, "stats", timeout=3)
        print("ok  stats stream")
    print("ALL E2E CHECKS PASSED")


asyncio.run(main())
