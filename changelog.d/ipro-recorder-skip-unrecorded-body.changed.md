- **Apache and nginx no longer read through responses the in-place recorder
  will not keep.** With the optimizer service in use, an HTML page, a response
  past the recording size limit, or one whose recording has already failed is
  now passed on without its body being read for the recorder: Apache no longer
  pulls a static file into memory for it, and nginx finishes it at once without
  taking room in its recording queue. Under load, HTML traffic can therefore no
  longer crowd out the recording of stylesheets, scripts and images
  (`ipro_daemon_record_dropped`). What is recorded is unchanged.
