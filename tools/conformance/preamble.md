# Lir - The Conformance Gate

What "supports MQTT" means for Lir: every normative statement of MQTT 5.0
and 3.1.1, by its identifier, against the test that proves it. Lir is one
broker made of a cluster, so every statement is a statement about the
cluster; a test of one that spans nodes runs on many nodes, one node being
only a cluster of one.

How it is kept:
- The statements are taken whole from the OASIS Standards as published
  (`mqtt-v5.0-os.html`, 252 statements; `mqtt-v3.1.1-os.html`, 141), each
  with the text before its identifier. 3.1.1's conformance appendix numbers
  some statements differently from its body (a known erratum), and its body
  misprints three identifiers (`3.3.1.-1`, `3-8.3-4`, `3.1.2.7`); the body's
  numbering is used, the misprints read as `3.3.1-1`, `3.8.3-4`, `3.1.2-7`.
  Two 5.0 statements carry their identifier only in the appendix
  (`MQTT-3.2.2-8`, `MQTT-4.2.0-1`).
- A statement is proven by a test whose name carries its identifier
  (`MQTT_3_1_2_11` for 5.0, `MQTT311_3_1_2_11` for 3.1.1), written from the
  statement and seen failing before the code. The table is generated from
  the test names; nothing here is judged by hand. Regenerate it with
  `python3 -I tools/conformance/gate.py docs/conformance.md lir/test/*_test.cpp`
  after adding a test; the statement lists beside that script came from
  the OASIS texts by `extract.py`.
- Lir was rewritten from scratch on 2026-11-20. The gate of the withdrawn
  code (its verdicts, read against tests that no longer exist) is in the
  history at `60cf78d`. What it found stands: of 5.0's statements 84 were
  tested then, and almost none across nodes.
