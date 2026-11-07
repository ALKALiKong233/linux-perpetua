# Proposal: hazards are brought up by name

Status: proposal, not implemented.

## Goal

Booting perpetua on a device must never power hardware that nobody has
brought up on that device. Charging comes in when someone says "let's bring
up charging" for that device, never as a side effect of "let's bring up the
device": a driver that loads because its compatible matched, finds a chip
that looks familiar but is wired differently, and burns something.

Every image is already built for one device, by one tool that takes the
device name. That build is where the rule lives. The kernel stays generic:
no `if (device == aston)`, no per-device `#define`, no runtime checks in
drivers.

## How a hazard gets in today

- Kconfig: `devices/config` gives a device everything defconfig builds for
  its vendor. A charger, amplifier or flash driver built as a module loads
  as soon as a node matches it.
- Device tree: SoC and PMIC `.dtsi` files and imported board trees can
  enable charger, flash or amplifier nodes on their own.
- Wrong device: when the bootloader's tree does not match the built-in
  tree's `/chosen/fdt-handoff`, the kernel boots the bootloader's tree as
  usual. Upstream drivers can then match vendor nodes on a device that
  perpetua knows nothing about.

## Proposal

### Hazard classes

`devices/hazards` lists the classes of hardware that can be damaged, and
for each the Kconfig symbols and device tree compatibles that belong to it:

| Class | Examples |
| --- | --- |
| charging | charger ICs, charge pumps, PMIC chargers, USB PD sinks that raise the input voltage |
| battery | fuel gauges and battery drivers that set charge limits |
| speaker | smart amplifiers, boost converters for speakers |
| flash | camera flash and torch LED drivers |
| cooling | fans, pumps |
| haptics | haptic drivers with their own boost |

The list is data. Adding a driver to one of those directories
(`drivers/power/supply`, the amplifier codecs, `drivers/leds/flash`, ...)
without classifying it fails `devices/check`, so a new hazard cannot enter
unnoticed.

### Bringing one up

`devices/<codename>/device` names what has been brought up on that device:

```sh
hazards="charging speaker"
```

Adding a word there is the bring-up step. It goes with the evidence in the
device's record: the stock values, what was measured, who confirmed it.

### What the build does

- `devices/config` forces off the Kconfig symbols of every class the device
  does not name, after all other fragments, so no fragment can bring them
  back. Those drivers are not built and cannot load.
- After the device tree is built, `devices/check` fails when an enabled node
  has a compatible of a class the device does not name, for example a
  charger node enabled by a PMIC `.dtsi`.
- Naming a class whose drivers the device does not enable is reported too,
  so the list stays true.

### Wrong device

A per-device image has no reason to boot any other tree. With
`ARM64_FDT_HANDOFF`, a bootloader tree that does not match the built-in
tree's `stock-compatible`, `stock-model` or `stock-bootargs` should stop the
boot before any driver probes, with a message on the console, instead of
booting the bootloader's tree. An image flashed on the wrong device then
does nothing at all.

## Out of scope

Rail voltages and GPIOs driven as outputs are needed from the first boot and
do not fit "bring up X". They stay in the review of the board device tree,
checked against the stock tree. A later step can compare them mechanically.

## Migration

Every existing device names what already runs on it before the check is
turned on, for example `hazards="charging speaker"` for aston, each backed
by its record. Then the check is enabled for all devices at once.
