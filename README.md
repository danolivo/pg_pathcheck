<p align="center">
  <img src="pics/banner-readme-dark.jpg" alt="pg_pathcheck banner" />
</p>

# pg_pathcheck

PostgreSQL extension that validates planner final Path tree state, detecting freed or corrupt memory by walking every reachable Path and checking NodeTags.

## Disclaimer

Most of the code in this repository — including the extension itself, the
CI workflow, the helper scripts, and portions of the documentation — was
generated in collaboration with a large language model (Claude). Every
change was reviewed and directed by a human before being committed, but
the prose and structure are largely machine-produced. Please read the
code rather than trusting the comments, and report any issues upstream.
