from emulator import runtime_state


class NeoPixel:
    def __init__(self, pin, count, bpp=3, timing=1):
        self.pin = pin
        self.count = int(count)
        self.bpp = int(bpp)
        self.timing = timing
        self._pixels = [(0, 0, 0)] * self.count
        runtime_state.init_led(self.count)

    def __len__(self):
        return self.count

    def __getitem__(self, index):
        return self._pixels[index]

    def __setitem__(self, index, color):
        normalized = tuple(int(component) for component in color)
        self._pixels[index] = normalized
        runtime_state.set_led(index, normalized)

    def fill(self, color):
        normalized = tuple(int(component) for component in color)
        self._pixels = [normalized] * self.count
        runtime_state.fill_led(normalized)

    def write(self):
        runtime_state.mark_led_write()
