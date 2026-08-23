# Security Policy

## Supported versions

| Version | Supported |
| --- | --- |
| 1.x | Yes |
| Earlier versions | No |

Security fixes are released as new immutable versions. Published tags and
release assets are not replaced.

## Reporting a vulnerability

Do not open a public issue, discussion, or pull request for a suspected
security vulnerability.

Use GitHub's
[private vulnerability reporting](https://github.com/shreshthkapai/wmux/security/advisories/new)
form. If the form is unavailable, ask the maintainer for a private contact
method without including vulnerability details in the public request.

Include as much of the following as possible:

- affected wmux version and commit;
- operating system, architecture, terminal host, and shell;
- reproduction steps or a minimal proof of concept;
- expected and observed behavior;
- security impact and affected trust boundary;
- whether the issue crosses local-user, IPC, process, or terminal boundaries;
- suggested mitigation, if known.

Please avoid accessing data that is not yours, disrupting other users, or
publishing details before a fix and coordinated disclosure are available.

## Response process

Reports are assessed by severity, reproducibility, and affected versions. The
maintainer will use the private advisory to coordinate investigation, credit,
fix review, and disclosure. Acknowledgement and remediation timing depend on
severity and maintainer availability; this policy does not promise a fixed
service-level deadline.

Once a fix is ready, the project will publish a new version and an advisory
when appropriate. Reporters who want credit should provide their GitHub
username in the private report.
