"""Match processes to the installed bundle paths owned by this project."""
from app_identity import APP_BUNDLE, RUNNER_BUNDLE
from urllib.parse import unquote, urlparse

BUNDLES = {APP_BUNDLE,
           RUNNER_BUNDLE}


def owned_processes(processes, apps):
    roots = [unquote(urlparse(app['url']).path).rstrip('/') + '/'
             for app in apps if app.get('bundleIdentifier') in BUNDLES]
    return [process for process in processes
            if any(unquote(urlparse(process.get('executable', '')).path).startswith(root)
                   for root in roots)]
