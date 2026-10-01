# name: Lava
# summary: Slow glowing lava blobs drift, stretch and morph across the entire display.
# author: Stipple
# tags: ambient, animation, relaxing, lava
# panel: 52x16

class App
  var last
  var tick

  def init()
    self.last = now_ms()
    self.tick = 0
  end

  def blob(cx, cy, size)
    var edge = rgb(65, 0, 25)
    var outer = rgb(135, 5, 35)
    var inner = rgb(235, 25, 40)
    var hot = rgb(255, 110, 35)
    var core = rgb(255, 190, 65)

    if size == 3
      # Big organic blob

      pixel(cx - 1, cy - 3, edge)
      pixel(cx,     cy - 3, outer)
      pixel(cx + 1, cy - 3, edge)

      pixel(cx - 2, cy - 2, edge)
      pixel(cx - 1, cy - 2, outer)
      pixel(cx,     cy - 2, inner)
      pixel(cx + 1, cy - 2, outer)
      pixel(cx + 2, cy - 2, edge)

      pixel(cx - 3, cy - 1, edge)
      pixel(cx - 2, cy - 1, outer)
      pixel(cx - 1, cy - 1, inner)
      pixel(cx,     cy - 1, hot)
      pixel(cx + 1, cy - 1, inner)
      pixel(cx + 2, cy - 1, outer)
      pixel(cx + 3, cy - 1, edge)

      pixel(cx - 3, cy, outer)
      pixel(cx - 2, cy, inner)
      pixel(cx - 1, cy, hot)
      pixel(cx,     cy, core)
      pixel(cx + 1, cy, hot)
      pixel(cx + 2, cy, inner)
      pixel(cx + 3, cy, outer)

      pixel(cx - 3, cy + 1, edge)
      pixel(cx - 2, cy + 1, outer)
      pixel(cx - 1, cy + 1, inner)
      pixel(cx,     cy + 1, hot)
      pixel(cx + 1, cy + 1, inner)
      pixel(cx + 2, cy + 1, outer)
      pixel(cx + 3, cy + 1, edge)

      pixel(cx - 2, cy + 2, edge)
      pixel(cx - 1, cy + 2, outer)
      pixel(cx,     cy + 2, inner)
      pixel(cx + 1, cy + 2, outer)
      pixel(cx + 2, cy + 2, edge)

      pixel(cx - 1, cy + 3, edge)
      pixel(cx,     cy + 3, outer)
      pixel(cx + 1, cy + 3, edge)

    elif size == 2
      # Medium blob

      pixel(cx - 1, cy - 2, edge)
      pixel(cx,     cy - 2, outer)
      pixel(cx + 1, cy - 2, edge)

      pixel(cx - 2, cy - 1, edge)
      pixel(cx - 1, cy - 1, outer)
      pixel(cx,     cy - 1, inner)
      pixel(cx + 1, cy - 1, outer)
      pixel(cx + 2, cy - 1, edge)

      pixel(cx - 2, cy, outer)
      pixel(cx - 1, cy, inner)
      pixel(cx,     cy, core)
      pixel(cx + 1, cy, inner)
      pixel(cx + 2, cy, outer)

      pixel(cx - 1, cy + 1, outer)
      pixel(cx,     cy + 1, hot)
      pixel(cx + 1, cy + 1, outer)

      pixel(cx, cy + 2, edge)

    else
      # Small droplet

      pixel(cx, cy - 1, outer)

      pixel(cx - 1, cy, outer)
      pixel(cx, cy, hot)
      pixel(cx + 1, cy, outer)

      pixel(cx, cy + 1, edge)
    end
  end

  def draw()
    var t = now_ms()

    if t - self.last >= 110
      self.last = t
      self.tick = self.tick + 1
    end

    # Very dark warm background
    clear(rgb(5, 0, 4))

    var p1 = self.tick % 48
    var y1 = 0

    if p1 < 24
      y1 = 14 - (p1 / 2)
    else
      y1 = 2 + ((p1 - 24) / 2)
    end

    var x1 = 9

    if (p1 % 16) < 5
      x1 = 8
    elif (p1 % 16) > 10
      x1 = 10
    end

    var s1 = 2

    if y1 > 10
      s1 = 3
    elif y1 < 5
      s1 = 2
    end

    self.blob(x1, y1, s1)

    var p2 = (self.tick + 17) % 56
    var y2 = 0

    if p2 < 28
      y2 = 14 - (p2 / 2)
    else
      y2 = ((p2 - 28) / 2)
    end

    var x2 = 25

    if (p2 % 20) < 7
      x2 = 24
    elif (p2 % 20) > 13
      x2 = 26
    end

    var s2 = 3

    if y2 < 5
      s2 = 2
    end

    self.blob(x2, y2, s2)

    var p3 = (self.tick + 31) % 52
    var y3 = 0

    if p3 < 26
      y3 = 13 - (p3 / 2)
    else
      y3 = (p3 - 26) / 2
    end

    var x3 = 42

    if (p3 % 18) < 6
      x3 = 41
    elif (p3 % 18) > 12
      x3 = 43
    end

    var s3 = 2

    if y3 > 10
      s3 = 3
    end

    self.blob(x3, y3, s3)

    var p4 = (self.tick + 9) % 38
    var y4 = 14 - (p4 / 3)

    if y4 < 1
      y4 = 1
    end

    var x4 = 17

    if (p4 % 10) < 5
      x4 = 18
    end

    self.blob(x4, y4, 1)

    var p5 = (self.tick + 24) % 43
    var y5 = 2 + (p5 / 4)

    if y5 > 14
      y5 = 14
    end

    var x5 = 34

    if (p5 % 12) < 6
      x5 = 33
    end

    self.blob(x5, y5, 1)

    line(0, 15, width() - 1, 15, rgb(100, 2, 25))

    var bx = 0
    while bx < width()
      if ((bx + self.tick) % 11) < 5
        pixel(bx, 15, rgb(210, 12, 35))
      end

      bx = bx + 1
    end
  end
end

return App()
