top = '.'
out = 'build'

def options(ctx):
    ctx.load('pebble_sdk')

def configure(ctx):
    ctx.load('pebble_sdk')

def build(ctx):
    ctx.load('pebble_sdk')
    binaries = []
    saved_env = ctx.env
    for platform in ctx.env.TARGET_PLATFORMS:
        ctx.env = ctx.all_envs[platform]
        ctx.set_group(ctx.env.PLATFORM_NAME)
        elf = '{}/pebble-app.elf'.format(ctx.env.BUILD_DIR)
        ctx.pbl_build(
            source=ctx.path.ant_glob('src/c/**/*.c'),
            target=elf,
            bin_type='app',
        )
        binaries.append({'platform': platform, 'app_elf': elf})
    ctx.env = saved_env
    ctx.set_group('bundle')
    ctx.pbl_bundle(binaries=binaries, js=[])
