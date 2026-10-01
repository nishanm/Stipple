# name: Game of Life
# summary: Conway's cells breed and die across the panel, reseeding when they stall.
# author: Stipple
# tags: animation, generative, classic
# panel: 52x16

import math

class App
  var w, h
  var cells, next    # flat width*height boolean arrays, current and scratch
  var stale, seen    # stall detection: population, and how long it held
  var last           # when the last generation ran, on the device clock

  def init()
    self.w = width()
    self.h = height()
    self.last = 0
    self.seed()
  end

  def seed()
    self.cells = []
    self.next = []
    var i = 0
    var total = self.w * self.h
    while i < total
      # About 30% filled gives lively first generations without saturating
      # into large blocks that immediately die of overcrowding.
      self.cells.push((math.rand() % 10) < 3)
      self.next.push(false)
      i += 1
    end
    self.stale = -1
    self.seen = 0
  end

  def step()

    var c = self.cells
    var nx = self.next
    var w = self.w
    var h = self.h
    var pop = 0

    var y = 0
    while y < h
      var up = y - 1
      if up < 0 up = h - 1 end
      var dn = y + 1
      if dn >= h dn = 0 end

      var rowUp = up * w
      var row = y * w
      var rowDn = dn * w

      var x = 0
      while x < w
        var lf = x - 1
        if lf < 0 lf = w - 1 end
        var rt = x + 1
        if rt >= w rt = 0 end

        var n = 0
        if c[rowUp + lf] n += 1 end
        if c[rowUp + x]  n += 1 end
        if c[rowUp + rt] n += 1 end
        if c[row + lf]   n += 1 end
        if c[row + rt]   n += 1 end
        if c[rowDn + lf] n += 1 end
        if c[rowDn + x]  n += 1 end
        if c[rowDn + rt] n += 1 end

        var alive = c[row + x]
        var live = (alive && (n == 2 || n == 3)) || (!alive && n == 3)
        nx[row + x] = live
        if live pop += 1 end
        x += 1
      end
      y += 1
    end

    var tmp = self.cells
    self.cells = self.next
    self.next = tmp

    # A steady population means the board has settled into still lifes and
    # blinkers. Reseed before it becomes wallpaper.
    if pop == self.stale
      self.seen += 1
      if self.seen > 40 || pop == 0
        self.seed()
      end
    else
      self.stale = pop
      self.seen = 0
    end
  end

  def draw()

    var now = now_ms()
    if now - self.last >= 125
      self.last = now
      self.step()
    end

    clear(rgb(0, 0, 0))

    var c = self.cells
    var w = self.w
    var i = 0
    var y = 0
    while y < self.h
      # The gradient is computed per row, so rgb() is called sixteen times a
      # frame rather than once per living cell.
      var colour = rgb(20, 150 + (y * 80) / self.h, 60)
      var x = 0
      while x < w
        if c[i] pixel(x, y, colour) end
        i += 1
        x += 1
      end
      y += 1
    end
  end
end

return App()
