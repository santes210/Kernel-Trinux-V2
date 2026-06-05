/* sleeps a few seconds so we can observe its priority in ps/top */
int main() {
    print("slow: starting\n");
    sleep(4000);
    print("slow: done\n");
    return 0;
}
