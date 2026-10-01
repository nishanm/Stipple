# name: Pong
# summary: You against the clock. The knob or the - and + buttons move your paddle; press to serve.
# author: Stipple
# tags: game, retro, interactive
# panel: 52x16

# @input exclusive
# @config speed number "Ball speed" default=3 min=1 max=6 help="Higher is faster, and the computer gets better too."

import math
import string

# The playable counterpart to Auto Pong, which rallies by itself because until
# now a script only ever received one button. It keeps its own paddle on the
# left; the computer has the right one and misses on purpose often enough to
# be worth beating.
#
# Middle button leaves, as it does everywhere. Nothing here can take it.
class App
  var bx, by, dx, dy
  var mine, theirs
  var scoreMe, scoreThem
  var serving, last
  var flash

  def init()
    self.scoreMe = 0
    self.scoreThem = 0
    self.mine = 6
    self.theirs = 6
    self.flash = 0
    self._serve(1)
  end

  def _paddleHeight()
    return 4
  end

  def _speed()
    var s = store.get("speed", 3)
    if s < 1 s = 1 end
    if s > 6 s = 6 end
    return s
  end

  def _serve(toward)
    self.bx = width() / 2
    self.by = height() / 2
    self.dx = toward
    # Never dead flat. A ball travelling along one row is a ball nobody has to
    # move for, and the first rally is exactly when somebody decides whether
    # this is worth playing.
    self.dy = (self.scoreMe + self.scoreThem) % 2 == 0 ? 1 : -1
    self.serving = true
  end

  def _moveMine(delta)
    self.mine += delta
    if self.mine < 0
      self.mine = 0
    end
    if self.mine > height() - self._paddleHeight()
      self.mine = height() - self._paddleHeight()
    end
  end

  # Every name this can receive is one `# @input exclusive` granted. The middle
  # button is not among them and cannot be: it is how somebody leaves.
  def on_button(name)
    if name == 'left' || name == 'plus'
      self._moveMine(-1)
    elif name == 'right' || name == 'minus'
      self._moveMine(1)
    elif name == 'select'
      # Serve if the ball is waiting, otherwise start a fresh match. One
      # button doing two things is fine when the state on screen says which.
      if self.serving
        self.serving = false
      else
        self.scoreMe = 0
        self.scoreThem = 0
        self._serve(1)
      end
    end
  end

  def _step()
    var h = height()
    var w = width()
    var reach = self._paddleHeight()

    self.bx += self.dx
    self.by += self.dy

    if self.by <= 0
      self.by = 0
      self.dy = 1
    end
    if self.by >= h - 1
      self.by = h - 1
      self.dy = -1
    end

    # Mine, on the left.
    if self.bx <= 2 && self.dx < 0
      if self.by >= self.mine && self.by < self.mine + reach
        self.dx = 1
        # Where it hit decides the angle, so the paddle is a tool rather than
        # a wall. Hitting with the end is how you aim.
        var offset = self.by - self.mine
        if offset == 0
          self.dy = -1
        elif offset == reach - 1
          self.dy = 1
        end
        self.flash = 6
      elif self.bx < 0
        self.scoreThem += 1
        self._serve(1)
        return
      end
    end

    # Theirs, on the right.
    if self.bx >= w - 3 && self.dx > 0
      if self.by >= self.theirs && self.by < self.theirs + reach
        self.dx = -1
      elif self.bx > w - 1
        self.scoreMe += 1
        self._serve(-1)
        return
      end
    end
  end

  # The computer tracks the ball, badly on purpose, and worse at low speeds so
  # the easy setting is actually easier.
  def _think()
    var target = self.by - self._paddleHeight() / 2
    var lag = 7 - self._speed()
    if math.abs(self.theirs - target) > lag
      if self.theirs < target
        self.theirs += 1
      else
        self.theirs -= 1
      end
    end
    if self.theirs < 0 self.theirs = 0 end
    if self.theirs > height() - self._paddleHeight()
      self.theirs = height() - self._paddleHeight()
    end
  end

  def draw()
    var w = width()
    var h = height()
    var now = now_ms()

    if self.last == nil self.last = now end
    var interval = 130 - self._speed() * 18
    if now - self.last >= interval
      self.last = now
      if !self.serving
        self._step()
        self._think()
      end
      if self.flash > 0 self.flash -= 1 end
    end

    # Net.
    var y = 0
    while y < h
      pixel(w / 2, y, rgb(28, 28, 34))
      y += 2
    end

    text(w / 2 - 11, 0, string.format("%d", self.scoreMe), rgb(70, 90, 70))
    text(w / 2 + 7, 0, string.format("%d", self.scoreThem), rgb(90, 70, 70))

    var mineColour = self.flash > 0 ? rgb(160, 255, 200) : rgb(80, 200, 255)
    rect_fill(1, self.mine, 1, self._paddleHeight(), mineColour)
    rect_fill(w - 2, self.theirs, 1, self._paddleHeight(), rgb(255, 130, 80))

    pixel(self.bx, self.by, rgb(255, 255, 255))

    if self.serving
      # Say what to press. A game that waits without saying so looks frozen.
      text(w / 2 - 12, 9, "PRESS", rgb(60, 60, 70))
    end
  end
end

return App()
