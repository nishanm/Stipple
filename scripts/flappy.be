# name: Flappy
# summary: One button, one pixel, gaps to fly through. Press to flap.
# author: Stipple
# tags: game, button
# panel: 52x16

# A 52x16 rebuild of the one-button pixel game. The original was written for a
# 32x8 panel, where a gap is three pixels and the whole board is two jumps
# wide; here there is room for a real gap, a readable score, and enough warning
# of the next pipe to react to it. Same idea, not the same code.
#
# Positions are in hundredths of a pixel so gravity is not all-or-nothing: at
# whole pixels the bird either hangs still or teleports, and there is no
# setting in between that feels like flying.

import math

class App
  var y, fall, pipeX, gapY, score, best, over, started

  def init()
    self.best = 0
    self.reset()
  end

  def reset()
    self.y = 600           # row 6, with room above and below
    self.fall = 0
    self.pipeX = width()
    self.gapY = 5
    self.score = 0
    self.over = false

    self.started = false
  end

  # The only control an app gets. Flap while playing, restart when dead.
  def on_button(name)
    if self.over
      self.reset()
      self.started = true
      self.fall = -90
    elif !self.started
      self.started = true
      self.fall = -90
    else

      self.fall = -90
    end
  end

  def draw()
    clear(rgb(0, 0, 0))

    if self.over
      text(2, 0, "score " + str(self.score), rgb(255, 255, 255))
      text(2, 8, "best " + str(self.best), rgb(120, 120, 120))
      return
    end

    if self.started
      self.step()
    end
    self.draw_pipe()

    if self.started
      pixel(8, self.y / 100, rgb(255, 200, 0))
    else
      # Hovering, with a hint. The bird bobs so the screen does not look
      # frozen - a still panel on a carousel reads as a crashed app.
      var bob = (elapsed_ms() / 250) % 2
      pixel(8, self.y / 100 + bob, rgb(255, 200, 0))
      text(14, 5, "press", rgb(70, 70, 70))
    end

    # Score in the corner, dim enough not to compete with the bird.
    text(44, 0, str(self.score % 10), rgb(45, 75, 45))
  end

  def step()
    self.fall += 8
    self.y += self.fall

    if self.y < 0
      self.y = 0
      self.fall = 0
    end
    if self.y > (height() - 1) * 100
      self.finish()
      return
    end

    self.pipeX -= 1
    if self.pipeX < -2
      self.pipeX = width() + 6

      self.gapY = 2 + (math.rand() % (height() - 7))
      self.score += 1
    end

    if self.pipeX <= 9 && self.pipeX >= 7
      var row = self.y / 100
      if row < self.gapY || row > self.gapY + 4
        self.finish()
      end
    end
  end

  def finish()
    self.over = true
    if self.score > self.best
      self.best = self.score
    end
  end

  def draw_pipe()
    if self.pipeX < 0 || self.pipeX >= width()
      return
    end
    var green = rgb(0, 160, 60)
    for y : 0 .. height() - 1
      if y < self.gapY || y > self.gapY + 4
        pixel(self.pipeX, y, green)
        pixel(self.pipeX + 1, y, green)
      end
    end
  end
end

return App()
