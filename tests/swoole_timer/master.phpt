--TEST--
swoole_timer: timer in master
--SKIPIF--
<?php
require __DIR__ . '/../include/skipif.inc'; ?>
--FILE--
<?php
require __DIR__ . '/../include/bootstrap.php';

use Swoole\Server;
use Swoole\Timer;

const RES_FILE = __DIR__ . '/result.txt';
file_put_contents(RES_FILE, '');

$pm = new ProcessManager();
$pm->setWaitTimeout(-1);
$pm->parentFunc = function ($pid) use ($pm) {
    $fp = fopen(RES_FILE, 'rw');
    while (!feof($fp)) {
        $line = fgets($fp);
        if ($line) {
            echo $line;
        }
    }
    if (IS_MAC_OS) {
        $pm->kill();
    }
};
$pm->childFunc = function () use ($pm) {
    $server = new Server('0.0.0.0', $pm->getFreePort(), SWOOLE_PROCESS);
    $server->set([
        'worker_num' => 1,
        'log_file' => '/dev/null',
    ]);
    $server->on('start', function (Server $server) use ($pm) {
        file_put_contents(RES_FILE, "start\n", FILE_APPEND);
        $id = Timer::tick(30, function () {
            file_put_contents(RES_FILE, "timer 1\n", FILE_APPEND);
        });
        Timer::after(90, function () use ($id, $server, $pm) {
            file_put_contents(RES_FILE, "timer 2\n", FILE_APPEND);
            Timer::clear($id);
            Timer::tick(10, function ($id) use ($server, $pm) {
                static $i = 0;
                file_put_contents(RES_FILE, "timer 3\n", FILE_APPEND);
                $i++;
                if ($i > 4) {
                    file_put_contents(RES_FILE, "end\n", FILE_APPEND);
                    Timer::clear($id);
                    $pm->wakeup();
                    $server->shutdown();
                }
            });
        });
    });
    $server->on('receive', function () {});
    $server->start();
};
$pm->childFirst();
$pm->run();
@unlink(RES_FILE);
?>
--EXPECT--
start
timer 1
timer 1
timer 1
timer 2
timer 3
timer 3
timer 3
timer 3
timer 3
end
