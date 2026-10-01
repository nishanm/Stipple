# name: Fireplace
# summary: Procedural fire, the whole panel. Embers, a coal bed and no two frames alike.
# author: Stipple
# tags: ambient, animation
# panel: 52x16

# Rebuilt for 52x16 from Galadril's Pixel Fireplace, which was written for a
# 32x8 panel.
#
# The physics is the same idea - a hot bed at the bottom, heat averaged upward
# from the row below, cooling that increases with height - but every constant
# had to move. On eight rows a flame reaches the top in three steps, so the
# cooling has to be gentle or there is no fire at all. On sixteen it has twice
# the distance, so it can cool harder and the flames get the tapered shape the
# small panel never had room for.
#
# It also runs on its own clock rather than on the frame rate: fire that
# simulates once per frame speeds up on a faster device, and this is the one
# app where "it looks wrong" is the only symptom you would ever get.

class App
  var a, b, seed, last, phase

  def init()
    self.seed = now_ms() + 4517
    self.last = 0
    self.phase = 0
    self.a = []
    self.b = []

    var seed = self.seed
    var i = 0
    while i < 832
      seed = (seed * 1103515245 + 12345) % 2147483647
      if seed < 0
        seed = -seed
      end
      var y = i / 52
      var h = 240 - (15 - y) * 17 + seed % 70 - 35
      if h < 0
        h = 0
      end
      self.a.push(h)
      self.b.push(h)
      i += 1
    end
    self.seed = seed
  end

  def rnd(n)
    self.seed = (self.seed * 1103515245 + 12345) % 2147483647
    if self.seed < 0
      self.seed = -self.seed
    end
    return self.seed % n
  end

  def update()
    var n = now_ms()
    if n - self.last < 70
      return
    end
    self.last = n

    var a = self.a
    var b = self.b
    var w = width()
    var bed = 15 * w
    var seed = self.seed

    # The combustion layer: hot everywhere, with cooler holes and white-hot
    # pockets. The holes are what stop it looking like a strip light.
    var x = 0
    while x < w
      seed = (seed * 1103515245 + 12345) % 2147483647
      if seed < 0
        seed = -seed
      end
      var r = seed % 512
      var h = 195 + r % 61
      if r % 8 == 0
        h = 80 + r % 80
      end
      a[bed + x] = h
      x += 1
    end

    var p = 0
    while p < 7
      seed = (seed * 1103515245 + 12345) % 2147483647
      if seed < 0
        seed = -seed
      end
      a[bed + 2 + seed % (w - 4)] = 255
      p += 1
    end

    var phase = self.phase
    self.phase = 1 - phase

    var y = 0
    while y < 15
      var row = y * w
      var belowRow = row + w
      var twoBelow = belowRow + w
      if y >= 14
        twoBelow = belowRow
      end

      if y % 2 != phase
        x = 0
        while x < w
          b[row + x] = a[row + x]
          x += 1
        end
        y += 1
        continue
      end

      x = 0
      while x < w
        var below = belowRow + x
        var h = a[below]

        if x > 0
          h += a[below - 1]
        else
          h += a[below]
        end

        if x < w - 1
          h += a[below + 1]
        else
          h += a[below]
        end

        h += a[twoBelow + x]
        h = h / 4

        seed = (seed * 1103515245 + 12345) % 2147483647
        if seed < 0
          seed = -seed
        end

        # Cooling rises with height. Sixteen rows can afford a real gradient,
        # which is what gives the flames their taper.
        var cool = 6 + seed % 14
        if y < 10
          cool += 6
        end
        if y < 5
          cool += 8
        end
        h -= cool

        if h < 0
          h = 0
        end

        var xx = x
        var turn = (seed / 16) % 5
        if turn == 0 && x > 0
          xx = x - 1
        elif turn == 4 && x < w - 1
          xx = x + 1
        end

        b[row + xx] = h
        x += 1
      end
      y += 1
    end

    x = 0
    while x < w
      b[bed + x] = a[bed + x]
      x += 1
    end

    self.seed = seed
    self.a = b
    self.b = a
  end

  def embers()
    # Sparks above the flames. Rare, because the point is that you catch one
    # occasionally rather than watch a fountain.
    if self.rnd(3) == 0
      var x = 4 + self.rnd(width() - 8)
      var y = self.rnd(7)
      if self.rnd(3) == 0
        pixel(x, y, 0xFFFF80)
      else
        pixel(x, y, 0xFF5000)
      end
    end
  end

  def bed()
    # The coals, drawn rather than simulated: they barely move, and a fixed
    # bed gives the fire something to sit on.
    var coals = [0x240800, 0x451000, 0x701500, 0x301000,
                 0x701500, 0xA02000, 0x451000, 0x8A1800,
                 0xD03000, 0x601000, 0xA02000, 0xF04000,
                 0x701500, 0xC02800, 0x501000, 0x901800,
                 0xD03000, 0x601000, 0x401000, 0x200800]
    var i = 0
    while i < size(coals)
      var x = 1 + i * 26 / 10
      if x < width()
        pixel(x, 15, coals[i])
      end
      i += 1
    end
  end

  def draw()
    self.update()

    clear(0x010000)

    var a = self.a
    var w = width()
    var i = 0
    var y = 0

    while y < 16
      var x = 0
      var runStart = 0
      var runColour = -1

      while x < w
        var h = a[i]

        var c = -1
        if h >= 18
          c = 0xFFFFD0
          if h < 55
            c = 0x260000
          elif h < 90
            c = 0x600000
          elif h < 125
            c = 0xB01000
          elif h < 165
            c = 0xFF2800
          elif h < 200
            c = 0xFF7000
          elif h < 230
            c = 0xFFC000
          elif h < 248
            c = 0xFFFF40
          end
        end

        if c != runColour
          if runColour >= 0
            rect_fill(runStart, y, x - runStart, 1, runColour)
          end
          runColour = c
          runStart = x
        end

        i += 1
        x += 1
      end

      # The run that reaches the right-hand edge.
      if runColour >= 0
        rect_fill(runStart, y, w - runStart, 1, runColour)
      end
      y += 1
    end

    self.embers()
    self.bed()
  end
end

return App()
