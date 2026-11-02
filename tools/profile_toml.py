# Profile parsing for player builds (plan.md, phase 7): the CLT's Python 3.9 lacks tomllib. Tomli 2.2.1 is its MIT-licensed backport, vendored with its license.
__all__ = ['tomllib']
try:
    import tomllib
except ModuleNotFoundError:
    from vendor import tomli as tomllib
