# name: Matrix
# summary: Matrix visuals
# author: Stipple
# tags: ambient, animation
# panel: 52x16

class App
  var last
  var tick

  def init()
    self.last = now_ms()
    self.tick = 0
  end

  def draw()
    var t = now_ms()

    if t - self.last >= 80
      self.last = t
      self.tick = self.tick + 1
    end

    clear(rgb(0, 0, 0))

    var w = width()
    var h = height()

    var x = 0
    while x < w
      var seed = x * 17 + 11

      var speed = 1 + (seed % 3)
      var len = 4 + ((seed * 7) % 8)
      var offset = (seed * 13) % 31

      var cycle = h + len + 10
      var head = ((self.tick * speed) + offset) % cycle
      head = head - len

      var i = 0
      while i < len
        var y = head - i

        if y >= 0 && y < h
          var c = rgb(0, 35, 8)

          if i == 0
            c = rgb(210, 255, 220)
          elif i == 1
            c = rgb(70, 255, 100)
          elif i == 2
            c = rgb(20, 210, 55)
          elif i == 3
            c = rgb(5, 150, 35)
          elif i < 6
            c = rgb(0, 90, 20)
          else
            c = rgb(0, 40, 8)
          end

          pixel(x, y, c)

          if ((seed + i * 5) % 4) == 0 && x + 1 < w
            pixel(x + 1, y, c)
          end
        end

        i = i + 1
      end

      x = x + 3
    end
  end
end

return App()
