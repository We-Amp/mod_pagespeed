- A volume path that is not a plain file is reported and not used: the
  server then runs with the in-memory cache. An idle cache no longer wakes
  up about 20 times a second (Cyclone cache library updated).
