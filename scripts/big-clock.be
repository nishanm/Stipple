# name: Big Clock
# summary: The time across the full width, with a bar that fills over the minute.
# author: Stipple
# tags: clock, time
# panel: 52x16

# Written for 52x16. A TC001 script would centre HH:MM in 32 pixels and stop
# there; here there is room for the time, the seconds, the date and a minute
# bar at once, which is most of the reason the wider panel is worth having.
#
# The 5x7 font advances six pixels per character, so a line holds eight. Every
# position below is chosen against that: nothing is centred by eye, because
# anything that overruns is silently clipped rather than wrapped and you only
# find out by looking at the panel.

import string

class App
  def draw()
    clear(rgb(0, 0, 0))

    # A device that has not synchronised its clock does not have a time, and
    # drawing 00:00 would be inventing one.
    if !time_known()
      text(8, 5, "no time", rgb(180, 60, 60))
      return
    end

    # "14:37" is five characters: 1 to 31.
    text(1, 0, string.format("%02d:%02d", hour(), minute()), rgb(255, 255, 255))

    # Seconds beside it, dim, 34 to 46. They move constantly and would pull
    # the eye off the digits that matter if they were the same weight.
    text(34, 0, string.format("%02d", second()), rgb(0, 110, 160))

    # "09.11" is five characters again: 1 to 31, under the time.
    text(1, 9, string.format("%02d.%02d", day(), month()), rgb(95, 95, 95))

    rect(33, 10, 19, 5, rgb(28, 28, 28))
    var filled = (second() * 17) / 60
    if filled > 0
      rect_fill(34, 11, filled, 3, rgb(0, 190, 255))
    end
  end
end

return App()
