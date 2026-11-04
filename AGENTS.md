# Agent notes

## Git and GitHub

Commit, push, and open pull requests as **kazetsukaiyuni** (yuni).

- Author: `kazetsukaiyuni <273437809+kazetsukaiyuni@users.noreply.github.com>`
- Set that author on this clone only. Do not change global git config, and do not commit as kazetsukai or kazetsukaimiko.
- Upstream is `kazetsukaimiko/gekko-yomi`. Yuni can pull and open PRs. Yuni cannot push to upstream.
- Push to the fork `kazetsukaiyuni/gekko-yomi` with the SSH key `~/.ssh/id_ed25519_yuni` (`IdentitiesOnly=yes`).
- Open the PR against `kazetsukaimiko/gekko-yomi` with yuni's GitHub token: `gh auth token --user kazetsukaiyuni`.
- Do not merge. Do not force-push unless the user asks.

## After CI is green

When a pull request has CI and that CI is green, start a monitor and wait for the user's review.

- Watch with `python3 ~/.grok/bundled/skills/long-running-background-tasks/watch_pr.py` and the PR URL.
- Run `gh` inside that watch as yuni (the token above), so the watcher sees yuni's PR.
- Wake on review comments, CI going red, merge conflicts, and the PR becoming ready for the user to merge.
- Stop when the PR is merged or closed.
- Do not merge it. Tell the user CI is green and the review is what remains.

A pull request that has no CI yet does not get this monitor. CI arrives with the test workflow, not with the interface skeleton.
