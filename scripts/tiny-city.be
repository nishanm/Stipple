# name: Tiny City
# summary: A living nighttime skyline with traffic, twinkling stars and shooting stars.
# author: Stipple
# tags: ambient, animation, city
# panel: 52x16

class App
  var last
  var tick
  var car1
  var car2

  def init()
    self.last = now_ms()
    self.tick = 0
    self.car1 = -4
    self.car2 = 45
  end

  def draw()
    var t = now_ms()

    if t - self.last >= 120
      self.last = t
      self.tick = self.tick + 1

      self.car1 = self.car1 + 1
      if self.car1 > width() + 3
        self.car1 = -4
      end

      self.car2 = self.car2 - 1
      if self.car2 < -4
        self.car2 = width() + 3
      end
    end

    # Pure black gives maximum contrast
    clear(rgb(0, 0, 0))

    pixel(2, 2, rgb(100, 120, 150))
    pixel(8, 1, rgb(220, 220, 190))
    pixel(15, 3, rgb(90, 110, 140))
    pixel(29, 1, rgb(180, 190, 210))
    pixel(37, 3, rgb(100, 120, 150))
    pixel(48, 1, rgb(160, 170, 190))

    if (self.tick % 10) < 5
      pixel(12, 1, rgb(255, 255, 220))
      pixel(40, 2, rgb(220, 230, 255))
    end

    pixel(45, 1, rgb(255, 240, 150))
    pixel(46, 1, rgb(255, 240, 150))
    pixel(44, 2, rgb(255, 240, 150))
    pixel(45, 2, rgb(255, 240, 150))
    pixel(44, 3, rgb(255, 230, 130))

    rect_fill(0, 9, 6, 5, rgb(5, 12, 22))

    # bright roof edge
    line(0, 8, 5, 8, rgb(20, 55, 75))

    pixel(1, 10, rgb(255, 190, 55))
    pixel(4, 10, rgb(70, 170, 220))

    pixel(1, 12, rgb(80, 170, 220))
    pixel(4, 12, rgb(255, 170, 40))

    rect_fill(7, 6, 7, 8, rgb(12, 7, 18))

    # stepped rooftop
    rect_fill(8, 5, 5, 1, rgb(20, 10, 28))
    rect_fill(9, 4, 3, 1, rgb(28, 12, 35))

    # purple edge
    line(7, 6, 7, 13, rgb(45, 20, 60))

    pixel(9, 7, rgb(255, 205, 70))
    pixel(12, 7, rgb(255, 130, 40))

    pixel(9, 9, rgb(80, 160, 220))
    pixel(12, 10, rgb(255, 190, 50))

    pixel(9, 12, rgb(255, 155, 40))
    pixel(12, 12, rgb(70, 150, 210))

    rect_fill(15, 8, 5, 6, rgb(4, 15, 16))

    line(15, 7, 19, 7, rgb(15, 65, 60))

    pixel(16, 9, rgb(255, 190, 55))
    pixel(18, 11, rgb(70, 190, 180))
    pixel(16, 12, rgb(255, 150, 35))

    rect_fill(21, 4, 9, 10, rgb(5, 8, 20))

    # stepped top
    rect_fill(22, 3, 7, 1, rgb(8, 12, 27))
    rect_fill(24, 2, 3, 1, rgb(12, 18, 34))

    # antenna
    pixel(25, 1, rgb(80, 90, 110))

    if (self.tick % 8) < 4
      pixel(25, 0, rgb(255, 20, 20))
    end

    # blue side highlight
    line(21, 4, 21, 13, rgb(18, 40, 75))

    # windows
    pixel(23, 5, rgb(255, 200, 65))
    pixel(26, 5, rgb(80, 170, 240))
    pixel(28, 6, rgb(255, 150, 35))

    pixel(23, 8, rgb(70, 150, 220))
    pixel(26, 8, rgb(255, 200, 60))

    pixel(28, 9, rgb(255, 175, 45))
    pixel(23, 11, rgb(255, 145, 35))
    pixel(26, 11, rgb(70, 160, 230))

    pixel(28, 12, rgb(255, 205, 65))

    rect_fill(31, 7, 7, 7, rgb(14, 7, 8))

    # roof / little spire
    line(31, 6, 37, 6, rgb(65, 25, 20))
    rect_fill(33, 5, 3, 1, rgb(50, 18, 15))
    pixel(34, 4, rgb(70, 30, 20))

    pixel(32, 8, rgb(255, 180, 40))
    pixel(35, 8, rgb(255, 110, 25))

    pixel(33, 10, rgb(70, 160, 220))
    pixel(36, 11, rgb(255, 185, 45))

    pixel(32, 12, rgb(255, 145, 30))

    rect_fill(39, 8, 13, 6, rgb(5, 10, 14))

    # cyan roof edge
    line(39, 7, 51, 7, rgb(15, 55, 65))

    pixel(40, 9, rgb(70, 170, 210))
    pixel(43, 9, rgb(255, 195, 55))
    pixel(46, 9, rgb(80, 180, 220))
    pixel(49, 9, rgb(255, 150, 35))

    pixel(41, 11, rgb(255, 175, 40))
    pixel(44, 11, rgb(60, 145, 190))
    pixel(47, 11, rgb(255, 200, 60))
    pixel(50, 11, rgb(70, 155, 200))

    pixel(40, 13, rgb(255, 145, 30))
    pixel(46, 13, rgb(70, 150, 200))
    pixel(49, 13, rgb(255, 180, 45))

    if (self.tick % 20) < 10
      pixel(17, 10, rgb(255, 190, 50))
      pixel(35, 12, rgb(70, 170, 220))
    else
      pixel(17, 10, rgb(20, 35, 30))
      pixel(35, 12, rgb(30, 20, 15))
    end

    rect_fill(0, 14, width(), 2, rgb(8, 8, 9))

    # pavement line
    line(0, 14, width() - 1, 14, rgb(45, 45, 48))

    var c1 = self.car1

    pixel(c1, 14, rgb(220, 30, 20))
    pixel(c1 + 1, 14, rgb(220, 30, 20))

    # headlight
    pixel(c1 + 2, 14, rgb(255, 240, 150))

    var c2 = self.car2

    # headlight
    pixel(c2, 15, rgb(255, 240, 160))

    pixel(c2 + 1, 15, rgb(30, 100, 220))
    pixel(c2 + 2, 15, rgb(30, 100, 220))

    var phase = self.tick % 100

    if phase < 10
      var sx = 1 + phase * 3
      var sy = 1 + (phase / 4)

      pixel(sx, sy, rgb(255, 255, 240))

      if sx > 0
        pixel(sx - 1, sy, rgb(120, 140, 130))
      end

      if sx > 1
        pixel(sx - 2, sy, rgb(40, 55, 50))
      end
    end
  end
end

return App()
