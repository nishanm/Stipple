# name: Tetris
# summary: Plays itself, forever. A sixteen-pixel-tall panel is exactly the shape of a well.
# author: Stipple
# tags: game, animation, retro
# panel: 52x16

# @config speed number "Frames per step" default=2 min=1 max=10 help="Lower is faster. The pieces fall a row per step."
# @config ghost boolean "Show where it will land" default=true

class App
  var W, H
  var well
  var SHAPES, COLS
  var piece, rot, px, py, colour
  var target, targetRot
  var tick, step, lines, ghost
  var flash

  def init()
    # Ten wide, full height, on the left. The rest of the panel is the score
    # board, which is how an arcade cabinet laid it out for the same reason:
    # the well is tall and thin and everything else is not.
    self.W = 10
    self.H = 16

    # Each shape as four rotations of four (x, y) pairs, so no rotation maths
    # happens at run time and no piece can rotate itself into a bad shape.
    self.SHAPES = [
      [[0,1, 1,1, 2,1, 3,1], [2,0, 2,1, 2,2, 2,3], [0,1, 1,1, 2,1, 3,1], [1,0, 1,1, 1,2, 1,3]],  # I
      [[0,0, 0,1, 1,1, 2,1], [1,0, 2,0, 1,1, 1,2], [0,1, 1,1, 2,1, 2,2], [1,0, 1,1, 0,2, 1,2]],  # J
      [[2,0, 0,1, 1,1, 2,1], [1,0, 1,1, 1,2, 2,2], [0,1, 1,1, 2,1, 0,2], [0,0, 1,0, 1,1, 1,2]],  # L
      [[0,0, 1,0, 0,1, 1,1], [0,0, 1,0, 0,1, 1,1], [0,0, 1,0, 0,1, 1,1], [0,0, 1,0, 0,1, 1,1]],  # O
      [[1,0, 2,0, 0,1, 1,1], [1,0, 1,1, 2,1, 2,2], [1,0, 2,0, 0,1, 1,1], [0,0, 0,1, 1,1, 1,2]],  # S
      [[1,0, 0,1, 1,1, 2,1], [1,0, 1,1, 2,1, 1,2], [0,1, 1,1, 2,1, 1,2], [1,0, 0,1, 1,1, 1,2]],  # T
      [[0,0, 1,0, 1,1, 2,1], [2,0, 1,1, 2,1, 1,2], [0,0, 1,0, 1,1, 2,1], [1,0, 0,1, 1,1, 0,2]]   # Z
    ]

    self.COLS = [
      rgb(0, 200, 220), rgb(60, 110, 230), rgb(235, 140, 40), rgb(230, 200, 40),
      rgb(70, 200, 90), rgb(180, 80, 210), rgb(230, 70, 70)
    ]

    self.well = []
    var i = 0
    while i < self.W * self.H
      self.well.push(-1)
      i += 1
    end

    # A stack to arrive into. An empty well takes about a minute of watching
    # before it looks like anything, and the carousel gives this eighteen
    # seconds - so the game starts in progress, the way you would find a
    # cabinet somebody had been playing.
    var row = self.H - 5
    while row < self.H
      var c = 0
      while c < self.W
        # Ragged, with a gap that moves, so it reads as a game rather than as
        # a wall. The bottom row keeps one column open or it would clear
        # itself on the first piece.
        var gap = (row * 3 + 2) % self.W
        if c != gap && (c + row) % 7 != 0
          self.well[row * self.W + c] = (c + row) % 7
        end
        c += 1
      end
      row += 1
    end

    self.step = store.get("speed", 2)
    self.ghost = store.get("ghost", true)
    self.tick = 0
    self.lines = 0
    self.flash = 0
    self._spawn()
  end

  def duration()
    return 20000
  end

  def _at(x, y)
    if x < 0 || x >= self.W || y >= self.H
      return 1
    end
    if y < 0
      return 0
    end
    return self.well[y * self.W + x] >= 0 ? 1 : 0
  end

  # The well is read directly here rather than through _at. _plan tests
  # roughly eight hundred placements per piece, and at four method calls a
  # test that was most of a frame's instruction budget - the peak frame sat
  # one heartbeat under the ceiling, which is a script that dies the first
  # time anything else is slightly slower.
  def _fits(shape, ox, oy)
    var W = self.W
    var H = self.H
    var w = self.well
    var i = 0
    while i < 8
      var x = ox + shape[i]
      var y = oy + shape[i + 1]
      if x < 0 || x >= W || y >= H
        return false
      end
      if y >= 0 && w[y * W + x] >= 0
        return false
      end
      i += 2
    end
    return true
  end

  def _spawn()
    # now_ms() rather than a seed: there is no random() in the sandbox, and a
    # fixed sequence would make every device play the identical game forever.
    self.piece = int(now_ms() / 7 + self.lines * 3) % 7
    self.rot = 0
    self.px = 3
    self.py = -2
    self.colour = self.COLS[self.piece]
    self._plan()
    if !self._fits(self.SHAPES[self.piece][0], self.px, 0)
      self._clearWell()
    end
  end

  def _clearWell()
    var i = 0
    while i < self.W * self.H
      self.well[i] = -1
      i += 1
    end
    self.lines = 0
  end

  # Try every rotation in every column and keep the best landing.
  #
  # Scored the way a person plays rather than the way a solver does: low is
  # good, holes are very bad, and a flat surface is worth something. It loses
  # eventually, which is the point - a player that never tops out would draw
  # the same picture for ever.
  def _plan()
    var bestScore = -99999
    self.target = self.px
    self.targetRot = 0

    var r = 0
    while r < 4
      var shape = self.SHAPES[self.piece][r]
      var x = -2
      while x < self.W
        if self._fits(shape, x, 0)
          # Drop it.
          var y = 0
          while self._fits(shape, x, y + 1)
            y += 1
          end
          var score = self._score(shape, x, y)
          if score > bestScore
            bestScore = score
            self.target = x
            self.targetRot = r
          end
        end
        x += 1
      end
      r += 1
    end
  end

  def _score(shape, ox, oy)
    var W = self.W
    var H = self.H
    var w = self.well
    var deep = 0
    var holes = 0
    var i = 0
    while i < 8
      var cx = ox + shape[i]
      var cy = oy + shape[i + 1]
      deep += cy
      # Anything empty directly beneath a block it cannot reach again.
      var below = cy + 1
      while below < H && w[below * W + cx] < 0
        holes += 1
        below += 1
      end
      i += 2
    end
    return deep * 3 - holes * 12
  end

  def _lock()
    var shape = self.SHAPES[self.piece][self.rot]
    var i = 0
    while i < 8
      var cx = self.px + shape[i]
      var cy = self.py + shape[i + 1]
      if cy >= 0 && cy < self.H && cx >= 0 && cx < self.W
        self.well[cy * self.W + cx] = self.piece
      end
      i += 2
    end

    var y = self.H - 1
    while y >= 0
      var full = true
      var x = 0
      while x < self.W
        if self.well[y * self.W + x] < 0
          full = false
          break
        end
        x += 1
      end
      if full
        var row = y
        while row > 0
          var c = 0
          while c < self.W
            self.well[row * self.W + c] = self.well[(row - 1) * self.W + c]
            c += 1
          end
          row -= 1
        end
        var c2 = 0
        while c2 < self.W
          self.well[c2] = -1
          c2 += 1
        end
        self.lines += 1
        self.flash = 4
      else
        y -= 1
      end
    end

    self._spawn()
  end

  def _advance()
    # Rotate first, then walk sideways, then fall - the order a person would
    # do it in, and it keeps the piece from clipping a wall mid-turn.
    if self.rot != self.targetRot
      var next = (self.rot + 1) % 4
      if self._fits(self.SHAPES[self.piece][next], self.px, self.py)
        self.rot = next
      end
      return
    end
    if self.px != self.target
      var dir = self.target > self.px ? 1 : -1
      if self._fits(self.SHAPES[self.piece][self.rot], self.px + dir, self.py)
        self.px += dir
        return
      end
    end
    if self._fits(self.SHAPES[self.piece][self.rot], self.px, self.py + 1)
      self.py += 1
    else
      self._lock()
    end
  end

  def draw()
    clear(rgb(0, 0, 0))

    self.tick += 1
    if self.tick >= self.step
      self.tick = 0
      self._advance()
    end
    if self.flash > 0
      self.flash -= 1
    end

    # The well, two pixels per cell across so it fills the height properly.
    var y = 0
    while y < self.H
      var x = 0
      while x < self.W
        var v = self.well[y * self.W + x]
        if v >= 0
          rect_fill(x * 2, y, 2, 1, self.COLS[v])
        end
        x += 1
      end
      y += 1
    end

    var shape = self.SHAPES[self.piece][self.rot]

    if self.ghost
      var gy = self.py
      while self._fits(shape, self.px, gy + 1)
        gy += 1
      end
      var i = 0
      while i < 8
        var cy = gy + shape[i + 1]
        if cy >= 0
          rect_fill((self.px + shape[i]) * 2, cy, 2, 1, rgb(30, 34, 40))
        end
        i += 2
      end
    end

    var j = 0
    while j < 8
      var cy = self.py + shape[j + 1]
      if cy >= 0
        rect_fill((self.px + shape[j]) * 2, cy, 2, 1, self.colour)
      end
      j += 2
    end

    # The wall of the well, then the score beside it.
    rect_fill(20, 0, 1, 16, rgb(24, 28, 34))

    var c = rgb(120, 130, 145)
    if self.flash > 0
      c = rgb(255, 255, 255)
    end
    text(23, 0, "LINES", rgb(60, 66, 78))
    text(23, 8, str(self.lines), c)
  end
end

return App()
