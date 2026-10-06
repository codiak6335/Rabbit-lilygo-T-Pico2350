# Compact two-board carrier — placement study

Open [`rabbit_compact_carrier.kicad_pro`](rabbit_compact_carrier.kicad_pro) in KiCad. This is a new project in the Rabbit repository; [`../proto`](../proto) remains untouched as a visual/mechanical reference. `proto` has two Pico 2 W footprints and cannot be used as the electrical design for the RP2350B-Plus-W and ESP32-C6-LCD-1.9 pair.

The first candidate outline is **88 × 58 mm**, compared with about 89.2 × 57 mm for `proto` and 115 × 92 mm for the earlier powered-carrier draft. The two existing modules and all future components are intended to mount on the **front/top face**. This does not mean single-layer copper: the file starts as a two-layer PCB. Both module USB-C sockets face the left board edge. The right-hand rectangles reserve space for the DROK, 12 V input, and LED/buzzer connection, but are **drawings, not populated footprints**.

J1/J2 represent the ESP's two 1×20 socket rows; J3/J4 represent the RP board's rows. The three verified UART/GND nets are assigned to the first three pads of J1/J3 but **are not routed**. All other pads are intentionally unconnected. The local `Rabbit_Carrier.pretty` footprint library is included so the project is self-contained. Its row pitch and pad size come from the prior carrier draft; the actual two boards, socket height, pin orientation, body envelopes, LCD overhang, antenna clearance, and USB cable clearance still need a full-size print/physical fit check.

**PLACEMENT ONLY — DO NOT FABRICATE OR POWER.** There is no KiCad schematic, safe power/USB coexistence circuit, protection circuitry, connector footprints, mounting-hole solution, DRC sign-off, or fabrication output. The compact right-hand reserve may need to grow when those parts are selected. In particular, do not connect DROK 5 V to either module while a PC USB cable is attached until the power-isolation design has been captured and verified.

Next design gate: confirm whether both module USB connections must remain connected to computers while the 12 V/DROK supply powers both boards. That choice determines the power/USB architecture and whether this outline can be retained.
