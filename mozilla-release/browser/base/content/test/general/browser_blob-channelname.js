/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */
Cu.import("resource://gre/modules/NetUtil.jsm");

function test() {
    var file = new File([new Blob(['test'], {type: 'text/plain'})], "test-name");
    var url = URL.createObjectURL(file);
    var channel = NetUtil.newChannel2(url,
                                      null,
                                      null,
                                      null,      // aLoadingNode
                                      Services.scriptSecurityManager.getSystemPrincipal(),
                                      null,      // aTriggeringPrincipal
                                      Ci.nsILoadInfo.SEC_NORMAL,
                                      Ci.nsIContentPolicy.TYPE_OTHER);

    is(channel.contentDispositionFilename, 'test-name', "filename matches");
}