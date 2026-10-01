# name: Langton's Ant
# summary: Two rules, ten thousand steps of chaos, and then it builds a road and leaves.
# author: Stipple
# tags: animation, generative, retro

# @config speed number "Steps per frame" default=40 min=5 max=200
# @config hue number "Colour" default=190 min=0 max=359

class App
  var W, H
  var cells
  var ax, ay, dir
  var steps, speed
  var INK, ANT

  def init()
    self.W = width()
    self.H = height()
    self.cells = []
    var i = 0
    while i < self.W * self.H
      self.cells.push(0)
      i += 1
    end
    self.speed = store.get("speed", 40)
    self.INK = self._hue(store.get("hue", 190), 150)
    self.ANT = rgb(255, 255, 255)
    self._restart()
  end

  def duration()
    return 20000
  end

  def _hue(h, v)
    var x = h % 360
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

  def _restart()
    var i = 0
    while i < self.W * self.H
      self.cells[i] = 0
      i += 1
    end
    self.ax = int(self.W / 2)
    self.ay = int(self.H / 2)
    self.dir = 0
    self.steps = 0
  end

  # The whole rule set. On a pale square turn right, on a dark one turn left;
  # flip the square either way and step forward. Everything the panel does
  # from here comes out of those two lines.
  def _step()
    var at = self.ay * self.W + self.ax
    if self.cells[at] == 0
      self.dir = (self.dir + 1) % 4
      self.cells[at] = 1
    else
      self.dir = (self.dir + 3) % 4
      self.cells[at] = 0
    end

    if self.dir == 0
      self.ay -= 1
    elif self.dir == 1
      self.ax += 1
    elif self.dir == 2
      self.ay += 1
    else
      self.ax -= 1
    end

    # Wrapping rather than a wall. The ant would otherwise walk off the edge
    # about a thousand steps in and there would be nothing left to watch.
    if self.ax < 0
      self.ax = self.W - 1
    elif self.ax >= self.W
      self.ax = 0
    end
    if self.ay < 0
      self.ay = self.H - 1
    elif self.ay >= self.H
      self.ay = 0
    end

    self.steps += 1
  end

  def draw()
    clear(rgb(0, 0, 0))

    var n = 0
    while n < self.speed
      self._step()
      n += 1
    end

    # On a panel this small the highway wraps round and eats its own road
    # long before it would on paper, so the pattern fills in and stops
    # changing. Starting again is more interesting than watching it sit.
    if self.steps > 60000
      self._restart()
    end

    var y = 0
    while y < self.H
      var x = 0
      while x < self.W
        if self.cells[y * self.W + x] != 0
          pixel(x, y, self.INK)
        end
        x += 1
      end
      y += 1
    end

    pixel(self.ax, self.ay, self.ANT)
  end
end

return App()
