# name: Binary Clock
# summary: Hours, minutes and seconds in columns of bits.
# author: Stipple
# tags: clock, time
# panel: 52x16

# Wants a wide panel. Six columns of bits with air between them needs about
# forty-six pixels, which is why this was never a TC001 app.

class App
  # One column of bits, least significant at the bottom - the way a binary
  # clock is read everywhere, and the opposite of how you would write it.
  def column(x, value, bits, colour)
    for bit : 0 .. bits - 1
      var y = 13 - bit * 3
      if (value >> bit) % 2 == 1
        rect_fill(x, y, 3, 3, colour)
      else

        rect(x, y, 3, 3, rgb(34, 34, 34))
      end
    end
  end

  def pair(x, value, colour)
    self.column(x, value / 10, 3, colour)
    self.column(x + 5, value % 10, 4, colour)
  end

  def draw()
    clear(rgb(0, 0, 0))

    if !time_known()
      text(8, 5, "no time", rgb(180, 60, 60))
      return
    end

    # 3 to 11, 21 to 29, 39 to 47. Wide gaps, because the grouping into
    # hours, minutes and seconds is the only thing making this readable.
    self.pair(3, hour(), rgb(255, 80, 80))
    self.pair(21, minute(), rgb(80, 255, 120))
    self.pair(39, second(), rgb(80, 150, 255))
  end
end

return App()
