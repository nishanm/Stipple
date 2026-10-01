# name: Starfield
# summary: Stars drifting past, at three depths. Quiet enough to leave on.
# author: Stipple
# tags: ambient, animation
# panel: 52x16

import math

class App
  var xs, ys, speeds

  def init()
    self.xs = []
    self.ys = []
    self.speeds = []
    # Twenty-four stars across 52 pixels is sparse enough to read as depth
    # rather than as noise. On a 32-pixel panel the same count is a blizzard.
    for i : 0 .. 23
      self.xs.push(math.rand() % 52)
      self.ys.push(math.rand() % 16)
      self.speeds.push(1 + (i % 3))
    end
  end

  def draw()
    clear(rgb(0, 0, 0))

    for i : 0 .. size(self.xs) - 1

      var speed = self.speeds[i]
      var x = self.xs[i] - speed
      if x < 0
        x = width() - 1
        self.ys[i] = math.rand() % height()
      end
      self.xs[i] = x

      var level = 40 + speed * 70
      pixel(x, self.ys[i], rgb(level, level, level))
    end
  end
end

return App()
