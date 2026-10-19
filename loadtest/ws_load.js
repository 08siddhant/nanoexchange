// k6 WebSocket load test.
//   k6 run loadtest/ws_load.js                       (defaults: 50 clients, 60s)
//   k6 run -e URL=ws://localhost:8080/ws -e VUS=200 -e DURATION=2m loadtest/ws_load.js
//
// Each virtual user opens one WebSocket, sends orders at ~20/s (under the
// server's 50 msg/s per-client rate limit) and measures the round trip from
// sending an order to receiving its ack.
import ws from 'k6/ws';
import { check } from 'k6';
import { Trend, Counter, Rate } from 'k6/metrics';

const URL = __ENV.URL || 'ws://localhost:8080/ws';
const VUS = parseInt(__ENV.VUS || '50', 10);
const DURATION = __ENV.DURATION || '60s';
const ORDERS_PER_SEC = parseFloat(__ENV.RATE || '20');
const SYMBOLS = ['AAPL', 'TSLA', 'INFY'];
const BASE = { AAPL: 230, TSLA: 250, INFY: 1550 };

const ackLatency = new Trend('order_ack_latency', true);
const acks = new Counter('order_acks');
const trades = new Counter('trade_messages');
const errors = new Rate('order_errors');

export const options = {
  scenarios: {
    traders: { executor: 'constant-vus', vus: VUS, duration: DURATION },
  },
  thresholds: {
    order_ack_latency: ['p(95)<50', 'p(99)<100'],   // milliseconds
    order_errors: ['rate<0.01'],
  },
};

export default function () {
  const sent = {};
  let seq = 0;
  const res = ws.connect(URL, {}, (socket) => {
    socket.on('open', () => {
      socket.setInterval(() => {
        const symbol = SYMBOLS[Math.floor(Math.random() * SYMBOLS.length)];
        const side = Math.random() < 0.5 ? 'buy' : 'sell';
        const id = `k${__VU}-${++seq}`;
        const order = { op: 'new', symbol, side, qty: 1 + Math.floor(Math.random() * 20), clOrdId: id };
        if (Math.random() < 0.8) {
          order.type = 'limit';
          const off = (Math.random() * 0.004 - 0.002) * BASE[symbol];
          order.price = Math.round((BASE[symbol] + off) * 100) / 100;
        } else {
          order.type = 'market';
        }
        sent[id] = Date.now();
        socket.send(JSON.stringify(order));
      }, 1000 / ORDERS_PER_SEC);

      // Clean up resting orders now and then so the book doesn't grow forever.
      socket.setInterval(() => {
        for (const s of SYMBOLS) socket.send(JSON.stringify({ op: 'cancel_all', symbol: s }));
      }, 5000);
    });

    socket.on('message', (raw) => {
      const m = JSON.parse(raw);
      if (m.type === 'ack' && sent[m.clOrdId]) {
        ackLatency.add(Date.now() - sent[m.clOrdId]);
        delete sent[m.clOrdId];
        acks.add(1);
        errors.add(false);
      } else if (m.type === 'error' && m.clOrdId) {
        errors.add(true);
      } else if (m.type === 'trades') {
        trades.add(1);
      }
    });

    socket.setTimeout(() => socket.close(), 1000 * 60 * 60);  // ended by k6 duration
  });
  check(res, { 'websocket upgraded (101)': (r) => r && r.status === 101 });
}
