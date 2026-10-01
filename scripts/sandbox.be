# name: Sandbox
# summary: Falling sand. It pours, it piles, it slumps - and the button shakes the whole thing loose.
# author: Stipple
# tags: animation, physics, interactive
# panel: 52x16

# @config hue number "Colour" default=30 min=0 max=359 help="0 red, 30 sand, 120 green, 210 blue. The grains vary a little either side of it."
# @config pour boolean "Keep pouring" default=true

# The grid is touched directly rather than through helpers. A method call per
# cell is affordable at eight hundred cells and is not affordable four times
# over, which is what a full panel of sand plus a shake adds up to - that
# combination went over the per-frame instruction budget and killed the script
# the moment somebody pressed the button.

class App
  var W, H
  var grid
  var PAL
  var spout, drift
  var shake
  var pouring

  def init()
    self.W = width()
    self.H = height()

    self.grid = []
    var i = 0
    while i < self.W * self.H
      self.grid.push(-1)
      i += 1
    end

    # Eight shades around the chosen hue. All one colour reads as a solid
    # block once it settles; the variation is what makes a pile look like one.
    var hue = store.get("hue", 30)
    self.PAL = []
    var k = 0
    while k < 8
      self.PAL.push(self._hue(hue - 14 + k * 4, 120 + k * 16))
      k += 1
    end

    self.pouring = store.get("pour", true)
    self.spout = int(self.W / 2)
    self.drift = 1
    self.shake = 0
  end

  def duration()
    return 20000
  end

  def _hue(h, v)
    var x = h % 360
    if x < 0
      x += 360
    end
    var seg = int(x / 60)
    var f = x % 60
    var up = int(f * v / 60)
    var dn = v - up
    if seg == 0
      return rgb(v, up, 0)
    elif seg == 1
      return rgb(dn, v, 0)
    elif seg == 2
      return rgb(0, v, up)
    elif seg == 3
      return rgb(0, dn, v)
    elif seg == 4
      return rgb(up, 0, v)
    end
    return rgb(v, 0, dn)
  end

  # A shake, not a reset: the pile slumps and reforms rather than vanishing.
  def on_button(name)
    if name != "select"
      return
    end
    self.shake = 8
  end

  def _settle(frame)
    var W = self.W
    var g = self.grid
    # The scan alternates direction each frame. Always sweeping one way builds
    # a visible lean, because the first grain of a row takes space the next
    # one then cannot use.
    var flip = frame % 2 == 0
    var first = flip ? -1 : 1

    # Bottom row upward, or a grain would fall the whole height in one frame
    # and the sand would look like rain.
    var y = self.H - 2
    while y >= 0
      var base = y * W
      var i = 0
      while i < W
        var x = i
        if flip
          x = W - 1 - i
        end
        i += 1

        var at = base + x
        var v = g[at]
        if v < 0
          continue
        end

        var below = at + W
        if g[below] < 0
          g[at] = -1
          g[below] = v
          continue
        end

        # Blocked underneath, so it may roll off the shoulder - but not every
        # frame. Sand that always rolls has no angle of repose and spreads
        # into a one-grain film instead of heaping.
        if (x * 7 + y * 3 + frame) % 3 != 0
          continue
        end

        var ax = x + first
        if ax >= 0 && ax < W && g[below + first] < 0
          g[at] = -1
          g[below + first] = v
          continue
        end
        var bx = x - first
        if bx >= 0 && bx < W && g[below - first] < 0
          g[at] = -1
          g[below - first] = v
        end
      end
      y -= 1
    end
  end

  def _pour(frame)
    # Slower than gravity. A spout moving a column per frame while a grain
    # falls a row per frame draws the stream as a diagonal line.
    if frame % 5 == 0
      self.spout += self.drift
      if self.spout <= 1 || self.spout >= self.W - 2
        self.drift = 0 - self.drift
      end
    end
    if self.grid[self.spout] < 0
      self.grid[self.spout] = self.PAL[int(frame / 3) % 8]
    end
  end

  # Once the sand reaches the top the spout is buried and nothing moves, which
  # looks broken rather than full. The floor leaks slowly instead.
  def _drain(frame)
    var g = self.grid
    var W = self.W
    var filled = 0
    var x = 0
    while x < W
      if g[2 * W + x] >= 0
        filled += 1
      end
      x += 1
    end
    if filled < 6
      return
    end
    g[(self.H - 1) * W + int(frame / 4) % W] = -1
  end

  def _shake(frame)
    var g = self.grid
    var W = self.W
    var y = 1
    while y < self.H
      var base = y * W
      var x = 0
      while x < W
        var at = base + x
        var v = g[at]
        if v >= 0 && g[at - W] < 0 && (x + y + frame) % 3 == 0
          g[at] = -1
          g[at - W] = v
        end
        x += 1
      end
      y += 1
    end
  end

  def draw()
    clear(rgb(0, 0, 0))
    var frame = int(now_ms() / 33)

    if self.shake > 0
      self.shake -= 1
      self._shake(frame)
    end

    if self.pouring
      self._pour(frame)
    end
    self._settle(frame)
    self._drain(frame)

    var g = self.grid
    var idx = 0
    var y = 0
    while y < self.H
      var x = 0
      while x < self.W
        var v = g[idx]
        if v >= 0
          pixel(x, y, v)
        end
        idx += 1
        x += 1
      end
      y += 1
    end
  end
end

return App()
