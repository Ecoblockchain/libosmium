
Some rules for contributing to this project:

* Please open a separate issue for each problem, question, or comment you have.
  Do not re-use existing issues for other topics, even if they are similar. This
  keeps issues small and manageable and makes it much easier to follow through
  and make sure each problem is taken care of.

* We'd love for you to send pull requests for fixes you have made or new features
  you have added. Please read the [notes for developers](NOTES_FOR_DEVELOPERS.md)
  beforehand which contains some coding guidelines.


### Releasing

1) Ensure all tests are passing

2) Update changelog

3) Tag a release

    git tag v0.1.0 -a -m "v0.1.0"

4) Upload release to github

    git push --tags

5) Publish to npm

First ensure only the desired files will be included:

    make test-package-npm

Then if the above output looks sane and only includes a few root files and the files in include/osmium do:

    npm publish