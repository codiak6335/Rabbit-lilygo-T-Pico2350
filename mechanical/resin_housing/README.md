# PETG housing with cast-resin viewing lid

Printable housing for the two Waveshare boards on the **88 × 58 mm carrier placement**, with a clear resin window over the ESP32 display. The assembled shell is **108 × 90 × 31.1 mm**, excluding the cable boots. Both cable entries accept the same relief neck; choose reliefs for **2.3, 3.0, 3.5 or 4.0 mm cable jacket diameters**.

This is a mechanically checked prototype for frequent splashes and brief submersion up to 3 ft. **Physical fit and leak testing have not been performed; it has no verified waterproof/IP rating.** Print the coupons first. The carrier placement and assembled module heights remain assumptions until measured on the actual hardware.

![Assembly and plan](assembly_preview.png)

## Files and print quantities

All STL dimensions are millimetres. Each STL is already placed on the print bed in its intended orientation. Import at 100% scale.

| Part | Quantity | Material / purpose |
| --- | ---: | --- |
| [Body](stl/body_petg.stl) | 1 | PETG; open side up; blind insert pockets, cord groove and PCB supports |
| [Lid frame](stl/lid_frame_petg.stl) | 1 | PETG; outside face down; doubles as the resin casting frame |
| [Window retainer](stl/window_retainer_petg.stl) | 1 | PETG; sealant groove faces up in the slicer, toward the lid in assembly |
| [PCB corner clip](stl/pcb_corner_clip_petg_print_4.stl) | 4 | PETG; supplied on its side to print the lip without supports |
| [Cable entry coupon](stl/cable_entry_fit_coupon_petg.stl) | 1 optional | PETG; reproduces the horizontal bore and 8 mm wall thickness |
| [Resin casting coupon](stl/resin_cast_fit_coupon_petg.stl) | 1 optional | PETG; small version of the cast window flange |
| [Coupon retainer](stl/resin_coupon_retainer_petg.stl) | 1 optional | PETG; tests mechanical retention and the resin/PETG seal |
| [Unused entry plug](stl/unused_entry_plug_tpu.stl) | As needed | TPU; seal into any unused entry |
| Cable reliefs below | 1 per cable | TPU; inside flange on the bed, cable bore vertical |

| Cable jacket outside diameter | Closed relief | Split wrap relief | Nominal bore |
| --- | --- | --- | ---: |
| 2.3 mm | [STL](stl/strain_relief_2_3mm_closed_tpu.stl) | [STL](stl/strain_relief_2_3mm_split_wrap_tpu.stl) | 2.5 mm |
| 3.0 mm | [STL](stl/strain_relief_3_0mm_closed_tpu.stl) | [STL](stl/strain_relief_3_0mm_split_wrap_tpu.stl) | 3.2 mm |
| 3.5 mm | [STL](stl/strain_relief_3_5mm_closed_tpu.stl) | [STL](stl/strain_relief_3_5mm_split_wrap_tpu.stl) | 3.7 mm |
| 4.0 mm | [STL](stl/strain_relief_4_0mm_closed_tpu.stl) | [STL](stl/strain_relief_4_0mm_split_wrap_tpu.stl) | 4.2 mm |

Closed reliefs can be fitted before feeding the cable through. Split wraps open along a 0.4 mm radial seam for already terminated cables; that seam must be filled and sealed. The specified wire sizes refer to the **complete insulated jacket**, not the conductor diameter. A common jacket around multiple conductors needs its own measured relief bore.

![Relief cross-sections](strain_relief_preview.png)

## Dimensions and hardware assumptions

See [dimensions.svg](dimensions.svg) for the plan. Coordinates below follow the carrier drawing: X right, Y down; Z up from the outside bottom of the enclosure.

| Feature | Nominal dimension |
| --- | --- |
| Carrier PCB | 88 × 58 × 1.6 mm |
| PCB underside | Z = 5.5 mm; 2.5 mm clearance over the 3 mm floor |
| Space above PCB | 20 mm to lid; 18 mm under the window retainer |
| Viewing opening | 46 × 26 mm, corner radius 1 mm |
| Viewing opening centre | Carrier X = 31, Y = 14 mm |
| Cast resin | 4 mm thick at the viewing opening; 58 × 38 × 2.4 mm rear flange, corner radius 3 mm |
| Resin amount | About 7.2 mL net; allow for mixing/cup losses |
| Window retainer | 64 × 44 × 2 mm; continuous 1.2 mm wide × 0.6 mm deep sealant groove |
| Cable holes | Ø8.6 mm through the 8 mm right wall |
| Cable centres | Carrier Y = 27 and 47 mm; Z = 15 mm |
| Relief neck / collar | Ø7.4 mm neck, 8.3 mm between collar faces; Ø12 mm collars |
| Glue clearance | 0.6 mm radial gap around relief neck; 0.1 mm nominal radial gap around cable |
| Relief length | 33.8 mm total; approximately 24 mm projects outside the case |
| Main gasket | 2 mm silicone cord in a 2.8 × 1.5 mm groove; nominal 25% squeeze at the flat rim stop |
| Gasket centreline length | About 343 mm; cut/join to the actual printed groove |

The ESP board envelope is 51 × 26 mm at carrier X=3.5, Y=1. The RP2350 envelope is 51 × 21 mm at X=3.5, Y=32.9. The display centre uses an approximate +2 mm X offset within the ESP module; confirm this on your board. The manufacturer's active display is 42.72 × 22.70 mm, which clears the modeled window. See [Waveshare dimensions](https://docs.waveshare.com/ESP32-C6-LCD-1.9).

The internal supports and four screw-down edge clips hold the carrier without drilling mounting holes in it. USB connectors remain inside; remove the lid for service. The cable collars occupy the unused strip along the carrier's right edge. Verify the real power components, wiring loops, header/socket stack and antenna clearance against these volumes before printing the body. The rendered module heights are illustrative references, not measured assemblies.

The additional space at the display end accommodates the resin flange and retainer. The other end accommodates the removable PCB clips. The rim provides room for the cord seal and lid screws outside it.

## Materials and fasteners

- PETG for body, lid, retainer and clips; this version is sized for PETG printing. A change to PP requires new fit and adhesive trials.
- TPU, starting with 95A, for reliefs and unused-port plugs. Softer TPU may ease installation, but fit must be checked on the coupon.
- Optically clear two-part casting epoxy suitable for a **4 mm section**, with the manufacturer's mixing ratio and full cure time. For example, read the [clear casting epoxy technical sheet](https://www.smooth-on.com/tb/files/EPOXACAST_690_692_TB.pdf) when choosing resin; optical clarity, sunlight exposure and adhesion need your own coupon trial.
- Flexible neutral-cure sealant compatible with the specific cured resin, PETG, TPU and cable jacket. Confirm adhesion and immersion suitability on the coupons.
- Approximately 350 mm of 2 mm silicone cord, with a sealed end joint, or a custom continuous silicone gasket matching the groove.
- Four M3 heat-set inserts, nominal 4.6 mm outer diameter × 4 mm long; the body pilot pockets are Ø4.4 × 4.2 mm. Match the actual insert supplier's hole recommendation before printing.
- Four M3 × 8 mm button-head machine screws. The lid has Ø6.4 × 1.8 mm head recesses; use heads that fit them.
- Four M2 × 5 mm plastic-tapping screws for the window retainer, and four M2 × 8 mm plastic-tapping screws for the PCB clips. Both use Ø1.7 mm blind pilot holes. The casting coupon uses four more M2 × 5 screws temporarily.

## Printing

Start with a 0.4 mm nozzle, 0.2 mm layers, six PETG perimeters and solid floor/lid layers. Use 100% infill for the thin frame, retainer and clips. Use four TPU perimeters and 100% infill; print the reliefs upright as supplied. These are starting settings, not a waterproof process specification.

The body and cable coupon have horizontal circular bores. Inspect sagging and clean the bore gently; qualify relief fit using the coupon printed with the same profile. The other parts have no enclosed support cavities. Keep supports out of the cord groove, casting pocket and cable bore. A small brim can help the upright coupon and TPU parts.

Inspect printed seams and layer bonding. Seal any porosity with a compatible thin coating and keep gasket lands flat and clean. FDM watertightness depends on the print process; see [Prusa's watertight-print experiments](https://blog.prusa3d.com/watertight-3d-printing-pt1-vases-cups-and-other-open-models_48949/). Do not treat a watertight STL mesh as evidence of a watertight print.

## Casting and assembly

1. Print the cable and resin coupons and the required relief diameter. Check the fit on the actual cable, collar insertion, glue filling, resin cure and sealant adhesion. The coupon has a 20 × 14 mm viewing opening with the same 4 mm optical thickness and 2.4 mm rear flange construction.
2. Print the full lid. Its outside face is the flat face that was on the print bed; its inside face contains the larger stepped casting pocket. Clean the pocket and prepare the PETG bond surfaces as required by the resin/sealant supplier.
3. Place the outside face **down on flat, release-treated glass**, leaving the stepped inside pocket facing up. Hold the frame flat and seal the temporary mould contact so resin cannot escape under it. Keep mould release off the permanent bond surfaces. Pour through the central opening and fill the larger rear pocket flush with the inside face. The glass forms the smooth optical face; the wider pocket forms the retaining flange. Cast with the electronics removed and allow full cure.
4. Clean flash from the window and inspect it. Fill the retainer's continuous groove with compatible sealant. The groove straddles the resin/PETG seam when the ring is installed. Put the ring against the inside lid, **groove toward the lid**, and use the four M2 × 5 screws. Tighten gently and cure the sealant. The rear ring supports inward water pressure on the resin flange; the outer PETG ledge prevents outward movement. The sealant closes the seam.
5. Install the four M3 inserts flush with the body rim. Avoid disturbing the adjacent cord groove. Fit the silicone cord, sealing its joined ends; its compression is set by the lid touching the flat rim.
6. Fit each TPU relief with the smaller flat flange inside and the tapered boot outside. Try folding the inner TPU collar through the coupon before committing to the case. Closed reliefs are installed before feeding the cable; split wraps can be opened around it. Seal the entire neck-to-wall annulus, cable-to-bore gap, and split seam over the collar/neck region. Use continuous intact cable jacket at the seal. Cure fully. Seal a TPU plug into an unused entry.
7. Leak-test the empty enclosure with dry paper inside: frequent splashes, then brief submersion progressing to the intended maximum of 3 ft. Gently move the cables during testing and inspect the window seam, cable entries, cord joint and printed walls. Fix and retest any ingress before installing electronics.
8. Place the carrier on the four corner supports. The four identical clips sit on the outside ledges with their raised lips pointing inward over the PCB edge; top clips face +Y and bottom clips face −Y. Use four M2 × 8 screws. Check real display height and lateral alignment, cable bend space, and that the retainer clears the module stack. Close using the four M3 lid screws evenly until the rim meets; avoid crushing the PETG or over-tightening plastic-tapping screws.

## Editing and regenerating

The editable design is [parameters.json](parameters.json) plus [generate_resin_housing.py](generate_resin_housing.py). Change carrier dimensions, assembled height, display centre or cable positions there. The generator exports STLs, dimension drawing, previews and [validation.json](validation.json). It checks single-body watertight meshes, winding, exported STL reloads, assembly collisions, print-bed placement, LCD opening clearance and isolation of the gasket from fasteners and the resin pocket.

```sh
python3 -m venv /tmp/rabbit-housing-venv
/tmp/rabbit-housing-venv/bin/pip install -r mechanical/resin_housing/requirements.txt
/tmp/rabbit-housing-venv/bin/python mechanical/resin_housing/generate_resin_housing.py
```

To generate a variant without overwriting these files:

```sh
/tmp/rabbit-housing-venv/bin/python mechanical/resin_housing/generate_resin_housing.py \
  --parameters /path/to/variant.json --output /tmp/rabbit-housing-variant
```

For a different board placement, adjust the module references and clips in the generator as well as the dimensions. A fit-check failure is a reason to inspect the changed geometry. The released files passed all geometric checks with the supplied parameters. Sealing, screw grip, resin adhesion and print tolerances still require the physical trials described above.
